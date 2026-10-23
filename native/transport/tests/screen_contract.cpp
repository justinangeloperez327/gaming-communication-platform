// Real SFU source grants and room isolation, using synthetic pixels only.
// No screen, camera, microphone, speaker, Qt window or physical peer is opened.
#include "livekit/livekit.h"
#include "livekit/audio_source.h"
#include "livekit/local_audio_track.h"
#include "livekit/room_delegate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
constexpr char voice_identity[] = "gcp-session-00000000-0000-0000-0000-000000000041";
constexpr char voice_track_name[] = "gcp-contract-voice-during-screen";
struct Failure final : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool value, const char* message) { if (!value) throw Failure(message); }

struct Seen {
    std::atomic<unsigned> subscriptions{0}, departures{0}, disconnected{0}, frames{0}, synthetic{0}, invalid{0}, audio{0};
    std::atomic<int> width{0}, height{0};
};
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
    std::shared_ptr<livekit::LocalAudioTrack> voice_track;
    Peer voice, listener, publisher, viewer, other;
    connect(voice, "GCP_MEDIA_PUBLISHER_TOKEN");
    connect(listener, "GCP_MEDIA_SUBSCRIBER_TOKEN");
    livekit::AudioStream::Options audio_options;
    audio_options.capacity = 8;
    listener.room.setOnAudioFrameCallback(voice_identity, voice_track_name,
        [&](const livekit::AudioFrame& frame) {
            if (frame.sampleRate() != 48000 || frame.numChannels() != 1) ++listener.seen.invalid;
            if (std::any_of(frame.data().begin(), frame.data().end(), [](auto sample) { return std::abs(static_cast<int>(sample)) > 500; }))
                ++listener.seen.audio;
        }, audio_options);
    connect(publisher, "GCP_MEDIA_SCREEN_PUBLISHER_TOKEN");
    connect(other, "GCP_MEDIA_SCREEN_OTHER_TOKEN");
    connect(viewer, "GCP_MEDIA_SCREEN_VIEWER_TOKEN");
    livekit::VideoStream::Options video_options;
    video_options.capacity = 2;
    viewer.room.setOnVideoFrameCallback(screen_identity, track_name,
        [&](const livekit::VideoFrame& frame, std::int64_t) {
            // WebRTC can adapt dimensions even with one encoding layer.
            // Validate real negotiated RGBA storage/content, not source size.
            viewer.seen.width = frame.width();
            viewer.seen.height = frame.height();
            const bool valid = frame.width() > 0 && frame.height() > 0 &&
                frame.width() <= 1920 && frame.height() <= 1080 &&
                frame.type() == livekit::VideoBufferType::RGBA &&
                frame.dataSize() == static_cast<std::size_t>(frame.width()) *
                    static_cast<std::size_t>(frame.height()) * 4;
            if (!valid) {
                ++viewer.seen.invalid;
            } else {
                const auto pixel = (static_cast<std::size_t>(frame.height() / 2) *
                    static_cast<std::size_t>(frame.width()) + static_cast<std::size_t>(frame.width() / 2)) * 4;
                // Lossy encoding permits small color differences; zero-filled
                // frames and subscription-only events cannot pass this check.
                if (frame.data()[pixel + 1] >= 100 && frame.data()[pixel + 1] <= 220 &&
                    frame.data()[pixel + 2] >= 30 && frame.data()[pixel + 2] <= 150)
                    ++viewer.seen.synthetic;
            }
            ++viewer.seen.frames;
        }, video_options);
    const auto local = publisher.room.localParticipant().lock();
    const auto watching = viewer.room.localParticipant().lock();
    const auto speaking = voice.room.localParticipant().lock();
    const auto second = other.room.localParticipant().lock();
    require(local && watching && speaking && second, "missing local participant");
    denied([&] { return local->publishVideoTrack("forbidden-camera", video, livekit::TrackSource::SOURCE_CAMERA); });
    denied([&] { return local->publishAudioTrack("forbidden-mic", audio, livekit::TrackSource::SOURCE_MICROPHONE); });
    denied([&] { return watching->publishVideoTrack("forbidden-viewer", video, livekit::TrackSource::SOURCE_SCREENSHARE); });
    denied([&] { return speaking->publishVideoTrack("forbidden-voice-screen", video, livekit::TrackSource::SOURCE_SCREENSHARE); });
    voice_track = speaking->publishAudioTrack(voice_track_name, audio, livekit::TrackSource::SOURCE_MICROPHONE);
    require(voice_track != nullptr, "authorized voice publication failed");
    // One explicit software layer makes this authorization fixture independent
    // of optional encoder/auto-simulcast choices. Product defaults are unchanged.
    screen = livekit::LocalVideoTrack::createLocalVideoTrack(track_name, video);
    other_screen = livekit::LocalVideoTrack::createLocalVideoTrack("gcp-contract-other-screen", other_video);
    livekit::TrackPublishOptions screen_options;
    screen_options.source = livekit::TrackSource::SOURCE_SCREENSHARE;
    screen_options.simulcast = false;
    screen_options.video_codec = livekit::VideoCodec::VP8;
    local->publishTrack(screen, screen_options);
    second->publishTrack(other_screen, screen_options);
    auto frame = livekit::VideoFrame::create(160, 90, livekit::VideoBufferType::RGBA);
    for (std::size_t i = 0; i < static_cast<std::size_t>(160 * 90); ++i) {
        frame.data()[i * 4] = 30;
        frame.data()[i * 4 + 1] = 160;
        frame.data()[i * 4 + 2] = 90;
        frame.data()[i * 4 + 3] = 255;
    }
    const auto until = Clock::now() + 8s;
    auto pcm = livekit::AudioFrame::create(48000, 1, 480);
    for (std::size_t i = 0; i < pcm.data().size(); ++i)
        pcm.data()[i] = static_cast<std::int16_t>(4000 * std::sin(2 * 3.141592653589793 * 1000 * static_cast<double>(i) / 48000));
    const auto feed_audio = [&] { audio->captureFrame(pcm, 1000); };
    unsigned tick = 0;
    while (Clock::now() < until) {
        feed_audio();
        if (tick++ % 5 == 0) {
            frame.data()[0] = static_cast<std::uint8_t>(tick);
            video->captureFrame(frame);
            other_video->captureFrame(frame);
        }
        std::this_thread::sleep_for(10ms);
    }
    std::cout << "GCP_SCREEN_STAGE:observed subscriptions=" << viewer.seen.subscriptions
              << " decoded=" << viewer.seen.frames << " invalid=" << viewer.seen.invalid
              << " synthetic=" << viewer.seen.synthetic
              << " width=" << viewer.seen.width << " height=" << viewer.seen.height
              << " voice=" << listener.seen.audio << std::endl;
    require(viewer.seen.synthetic >= 5 && viewer.seen.invalid == 0, "no valid decoded synthetic video");
    require(publisher.seen.subscriptions == 0 && other.seen.subscriptions == 0,
        "publish-only identity received another share");
    require(voice.seen.subscriptions == 0 && voice.seen.disconnected == 0,
        "screen publication affected separate voice room");
    require(listener.seen.audio >= 10 && listener.seen.invalid == 0, "voice did not coexist with screen video");
    std::cout << "GCP_SCREEN_STAGE:source_grants_and_decoded_video" << std::endl;
    std::cout << "GCP_SCREEN_READY_FOR_EVICTION" << std::endl;
    // Runner verifies server track ownership then removes this exact publisher.
    wait_for([&] { feed_audio(); return publisher.seen.disconnected > 0; }, "screen publisher not disconnected");
    wait_for([&] { feed_audio(); return viewer.seen.departures > 0; }, "screen departure not observed");
    const auto before = listener.seen.audio.load();
    const auto after_stop = Clock::now() + 1500ms;
    while (Clock::now() < after_stop) { feed_audio(); std::this_thread::sleep_for(10ms); }
    require(listener.seen.audio >= before + 10, "voice stopped after screen removal");
    require(voice.seen.disconnected == 0 && listener.seen.disconnected == 0 && viewer.seen.disconnected == 0 && other.seen.disconnected == 0,
        "screen stop disconnected unrelated peer");
    viewer.room.clearOnVideoFrameCallback(screen_identity, track_name);
    voice.room.disconnect();
    listener.room.disconnect();
    viewer.room.disconnect();
    other.room.disconnect();
    std::cout << "GCP_SCREEN_CONTRACT:passed decoded_frames=" << viewer.seen.frames
              << " synthetic_frames=" << viewer.seen.synthetic
              << " voice_frames=" << listener.seen.audio
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
