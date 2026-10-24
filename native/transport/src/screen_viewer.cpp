#include "screen_viewer.h"
#include "livekit/livekit.h"
#include "livekit/room_delegate.h"
#include "livekit/remote_participant.h"
#include "livekit/remote_track_publication.h"
#include "livekit/video_stream.h"
#include <thread>
#include <stdexcept>
#include <atomic>

namespace gcp::transport {
namespace {
struct Events final : livekit::RoomDelegate {
    Events(ViewControl& value, std::string publisher) : control(value), identity(std::move(publisher)) {}
    ViewControl& control;
    const std::string identity;
    std::mutex mutex;
    std::shared_ptr<livekit::Track> candidate;
    bool selected(const livekit::RemoteParticipant* participant) const {
        return participant && participant->identity() == identity;
    }
    void onReconnecting(livekit::Room&, const livekit::ReconnectingEvent&) override { control.stop(); }
    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override { control.stop(); }
    void onParticipantDisconnected(livekit::Room&, const livekit::ParticipantDisconnectedEvent& e) override {
        if (selected(e.participant)) control.stop();
    }
    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& e) override {
        if (!selected(e.participant) || !e.publication || !e.track ||
            e.publication->kind() != livekit::TrackKind::KIND_VIDEO ||
            e.publication->source() != livekit::TrackSource::SOURCE_SCREENSHARE ||
            e.track->sid() != e.publication->sid() || !screen_track_sid(e.track->sid())) { control.stop(); return; }
        std::lock_guard lock(mutex);
        if (candidate && candidate->sid() != e.track->sid()) { control.stop(); return; }
        candidate = e.track;
    }
    void onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent& e) override {
        if (selected(e.participant)) control.stop();
    }
    void onTrackUnpublished(livekit::Room&, const livekit::TrackUnpublishedEvent& e) override {
        if (selected(e.participant)) control.stop();
    }
    void onTrackSubscriptionFailed(livekit::Room&, const livekit::TrackSubscriptionFailedEvent& e) override {
        if (selected(e.participant)) control.stop();
    }
    std::shared_ptr<livekit::Track> track() { std::lock_guard lock(mutex); return candidate; }
};
}
void view_screen(ViewControl& control, const ViewGrant& grant,
                 const ViewEvent& event, const ViewPixels& pixels) {
    Events events(control, grant.publisher); // Must outlive Room and its callbacks.
    livekit::Room room;
    room.setDelegate(&events);
    struct Stop { ViewControl& control; ~Stop() { control.stop(); } } stop{control};
    livekit::RoomOptions options;
    options.auto_subscribe = false;
    options.join_retries = 0;
    options.connect_timeout = std::chrono::seconds(10);
    if (!control.alive(screen_now_ms())) return;
    if (!room.connect(grant.url, grant.token, options)) throw std::runtime_error("Viewer join failed");
    const auto local = room.localParticipant().lock();
    if (!local || local->identity() != grant.identity || room.roomInfo().name != grant.room)
        throw std::runtime_error("Viewer scope mismatch");
    if (!control.alive(screen_now_ms())) return;
    event("connected");
    std::shared_ptr<livekit::Track> track;
    std::string sid;
    while (control.alive(screen_now_ms())) {
        sid = control.track_sid(); track = events.track();
        if (!sid.empty() && track) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!control.alive(screen_now_ms())) return;
    if (!track || track->sid() != sid) throw std::runtime_error("Viewer track mismatch");
    livekit::VideoStream::Options stream_options;
    stream_options.capacity = 1; stream_options.format = livekit::VideoBufferType::RGBA;
    auto stream = livekit::VideoStream::fromTrack(track, stream_options);
    if (!stream) throw std::runtime_error("Video stream unavailable");
    // close() wakes read() on a static/failed stream. Only this closer calls it
    // concurrently; the destructor runs after join, avoiding concurrent close.
    std::atomic<bool> done{false};
    std::thread closer([&] {
        while (!done.load() && control.alive(screen_now_ms()))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!done.load()) { try { stream->close(); } catch (...) { control.stop(); } }
    });
    struct Join {
        std::atomic<bool>& done; std::thread& thread;
        ~Join() { done.store(true); thread.join(); }
    } join{done, closer};
    event("subscribed " + sid);
    std::uint64_t sequence = 0;
    while (control.alive(screen_now_ms())) {
        if (!control.take_credit()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        livekit::VideoFrameEvent frame;
        if (!stream->read(frame)) { control.stop(); break; }
        if (!control.alive(screen_now_ms())) break;
        const auto width = frame.frame.width(), height = frame.frame.height();
        const auto rotation = static_cast<int>(frame.rotation);
        if (width < 1 || width > 1920 || height < 1 || height > 1080 ||
            frame.frame.type() != livekit::VideoBufferType::RGBA ||
            frame.frame.dataSize() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4 ||
            (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
            throw std::runtime_error("Invalid decoded frame");
        pixels(++sequence, frame.frame, rotation);
    }
}
} // namespace gcp::transport
