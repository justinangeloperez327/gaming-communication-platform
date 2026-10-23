// Real SFU source grants and room isolation, using synthetic pixels only.
// No screen, camera, microphone, speaker, Qt window or physical peer is opened.
#include "livekit/livekit.h"
#include "livekit/audio_source.h"
#include "livekit/local_audio_track.h"
#include "livekit/room_delegate.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr char screen_identity[] = "gcp-screen-publish-00000000-0000-0000-0000-000000000071";
constexpr char track_name[] = "gcp-contract-screen";
struct Failure final : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool value, const char* message) { if (!value) throw Failure(message); }

struct Seen { std::atomic<unsigned> subscriptions{0}, departures{0}, disconnected{0}, frames{0}, invalid{0}; };
struct Events final : livekit::RoomDelegate {
    explicit Events(Seen& state) : seen(state) {}
    Seen& seen;
    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent&) override { ++seen.subscriptions; }
    void onParticipantDisconnected(livekit::Room&, const livekit::ParticipantDisconnectedEvent&) override { ++seen.departures; }
    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override { ++seen.disconnected; }
};
// Destruction order keeps delegate/callback state alive until Room joins readers.
struct Peer {
    Seen seen;
    Events events{seen};
    livekit::Room room;
    Peer() { room.setDelegate(&events); }
};

void connect(Peer& peer, const char* variable) {
    const auto* token = std::getenv(variable);
    require(token && *token, "missing disposable token");
    livekit::RoomOptions options;
    options.auto_subscribe = true; // deliberately request more than publisher grants permit
    options.connect_timeout = 15s;
    require(peer.room.connect("ws://127.0.0.1:17880", token, options), "loopback join failed");
}

template<class Function> void denied(Function action) {
    bool rejected = false;
    try { rejected = !action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "forbidden track publication succeeded");
}

template<class Predicate> void wait_for(Predicate predicate, const char* message) {
    const auto end = Clock::now() + 10s;
    while (!predicate() && Clock::now() < end) std::this_thread::sleep_for(10ms);
    require(predicate(), message);
}

int contract() {
    // Keep sources/tracks alive through Room teardown, including unwinding.
    auto video = std::make_shared<livekit::VideoSource>(160, 90);
    auto other_video = std::make_shared<livekit::VideoSource>(160, 90);
    auto audio = std::make_shared<livekit::AudioSource>(48000, 1, 0);
    std::shared_ptr<livekit::LocalVideoTrack> screen, other_screen;
    Peer voice, publisher, viewer, other;
    connect(voice, "GCP_MEDIA_PUBLISHER_TOKEN");
    connect(publisher, "GCP_MEDIA_SCREEN_PUBLISHER_TOKEN");
    connect(other, "GCP_MEDIA_SCREEN_OTHER_TOKEN");
    connect(viewer, "GCP_MEDIA_SCREEN_VIEWER_TOKEN");
    viewer.room.setOnVideoFrameCallback(screen_identity, track_name,
        [&](const livekit::VideoFrame& frame, std::int64_t) {
            if (frame.width() != 160 || frame.height() != 90) ++viewer.seen.invalid;
            ++viewer.seen.frames;
        });
    const auto local = publisher.room.localParticipant().lock();
    const auto watching = viewer.room.localParticipant().lock();
    const auto speaking = voice.room.localParticipant().lock();
    const auto second = other.room.localParticipant().lock();
    require(local && watching && speaking && second, "missing local participant");
    denied([&] { return local->publishVideoTrack("forbidden-camera", video, livekit::TrackSource::SOURCE_CAMERA); });
    denied([&] { return local->publishAudioTrack("forbidden-mic", audio, livekit::TrackSource::SOURCE_MICROPHONE); });
    denied([&] { return watching->publishVideoTrack("forbidden-viewer", video, livekit::TrackSource::SOURCE_SCREENSHARE); });
    denied([&] { return speaking->publishVideoTrack("forbidden-voice-screen", video, livekit::TrackSource::SOURCE_SCREENSHARE); });
    screen = local->publishVideoTrack(track_name, video, livekit::TrackSource::SOURCE_SCREENSHARE);
    other_screen = second->publishVideoTrack("gcp-contract-other-screen", other_video, livekit::TrackSource::SOURCE_SCREENSHARE);
    require(screen && other_screen, "authorized screen source rejected");
    auto frame = livekit::VideoFrame::create(160, 90, livekit::VideoBufferType::RGBA);
    for (std::size_t i = 0; i < static_cast<std::size_t>(160 * 90); ++i) {
        frame.data()[i * 4] = 30;
        frame.data()[i * 4 + 1] = 160;
        frame.data()[i * 4 + 2] = 90;
        frame.data()[i * 4 + 3] = 255;
    }
    const auto until = Clock::now() + 4s;
    while (Clock::now() < until) {
        video->captureFrame(frame);
        other_video->captureFrame(frame);
        std::this_thread::sleep_for(50ms);
    }
    require(viewer.seen.frames >= 5 && viewer.seen.invalid == 0, "no valid decoded synthetic video");
    require(publisher.seen.subscriptions == 0 && other.seen.subscriptions == 0,
        "publish-only identity received another share");
    require(voice.seen.subscriptions == 0 && voice.seen.disconnected == 0,
        "screen publication affected separate voice room");
    std::cout << "GCP_SCREEN_STAGE:source_grants_and_decoded_video" << std::endl;
    std::cout << "GCP_SCREEN_READY_FOR_EVICTION" << std::endl;
    // Runner verifies server track ownership then removes this exact publisher.
    wait_for([&] { return publisher.seen.disconnected > 0; }, "screen publisher not disconnected");
    wait_for([&] { return viewer.seen.departures > 0; }, "screen departure not observed");
    require(voice.seen.disconnected == 0 && viewer.seen.disconnected == 0 && other.seen.disconnected == 0,
        "screen stop disconnected unrelated peer");
    viewer.room.clearOnVideoFrameCallback(screen_identity, track_name);
    voice.room.disconnect();
    viewer.room.disconnect();
    other.room.disconnect();
    std::cout << "GCP_SCREEN_CONTRACT:passed decoded_frames=" << viewer.seen.frames
              << " physical_capture_verified=false desktop_sharing_verified=false" << std::endl;
    return 0;
}
} // namespace

int main() {
    livekit::initialize(livekit::LogLevel::Warn);
    try {
        const auto result = contract();
        livekit::shutdown();
        return result;
    } catch (const Failure& error) {
        std::cout << "GCP_SCREEN_CONTRACT:failed " << error.what() << std::endl;
    } catch (const std::exception&) {
        // Never echo an SDK exception: it could contain a credential.
        std::cout << "GCP_SCREEN_CONTRACT:failed" << std::endl;
    }
    livekit::shutdown();
    return 1;
}
