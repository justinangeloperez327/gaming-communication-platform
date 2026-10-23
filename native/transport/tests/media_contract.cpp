// Real loopback SFU/SDK contract. Synthetic PCM only: never opens PlatformAudio,
// a microphone, speakers, camera or capture device. Not an audibility benchmark.
#include "livekit/livekit.h"
#include "livekit/audio_frame.h"
#include "livekit/audio_source.h"
#include "livekit/local_audio_track.h"
#include "livekit/room_delegate.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char publisher_identity[] = "gcp-session-00000000-0000-0000-0000-000000000041";
constexpr char track_name[] = "gcp-contract-synthetic-microphone";
constexpr double pi = 3.14159265358979323846;

struct ContractFailure final : std::runtime_error { using std::runtime_error::runtime_error; };

void require(bool value, const char* message) {
    if (!value) throw ContractFailure(message);
}

struct Observations {
    std::atomic<unsigned> subscriptions{0}, departures{0}, disconnections{0};
    std::atomic<unsigned> muted{0}, unmuted{0}, frames{0}, tone_frames{0}, invalid_frames{0};

    void frame(const livekit::AudioFrame& audio) {
        ++frames;
        const auto& samples = audio.data();
        if (audio.sampleRate() != 48000 || audio.numChannels() != 1 || samples.empty() ||
            audio.samplesPerChannel() <= 0 || samples.size() != static_cast<std::size_t>(audio.samplesPerChannel())) {
            ++invalid_frames;
            return;
        }
        // A decoded 1 kHz component distinguishes actual transported content
        // from zero-filled buffers, comfort noise and subscription-only events.
        double real = 0, imaginary = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double phase = 2 * pi * 1000 * static_cast<double>(i) / 48000;
            real += samples[i] * std::cos(phase);
            imaginary += samples[i] * std::sin(phase);
        }
        if (2 * std::hypot(real, imaginary) / static_cast<double>(samples.size()) > 250) ++tone_frames;
    }
};

struct Events final : livekit::RoomDelegate {
    explicit Events(Observations& observations) : seen(observations) {}
    Observations& seen;
    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& event) override {
        if (event.track && event.track->kind() == livekit::TrackKind::KIND_AUDIO) ++seen.subscriptions;
    }
    void onParticipantDisconnected(livekit::Room&, const livekit::ParticipantDisconnectedEvent&) override { ++seen.departures; }
    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override { ++seen.disconnections; }
    void onTrackMuted(livekit::Room&, const livekit::TrackMutedEvent&) override { ++seen.muted; }
    void onTrackUnmuted(livekit::Room&, const livekit::TrackUnmutedEvent&) override { ++seen.unmuted; }
};

// Room destruction joins its frame readers before the delegate/state die.
struct Peer {
    Observations seen;
    Events events{seen};
    livekit::Room room;
    Peer() { room.setDelegate(&events); }
};

void connect(Peer& peer, const char* token_name) {
    const auto* token = std::getenv(token_name);
    require(token && *token, "missing disposable participant token");
    livekit::RoomOptions options;
    options.auto_subscribe = true;
    options.connect_timeout = 15s;
    require(peer.room.connect("ws://127.0.0.1:17880", token, options), "loopback room connect failed");
}

template<class Predicate>
void wait_for(Predicate predicate, const char* failure) {
    const auto deadline = Clock::now() + 10s;
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(10ms);
    require(predicate(), failure);
}

void pump(livekit::AudioSource& source, std::chrono::milliseconds duration) {
    auto frame = livekit::AudioFrame::create(48000, 1, 480);
    for (std::size_t i = 0; i < frame.data().size(); ++i)
        frame.data()[i] = static_cast<std::int16_t>(4000 * std::sin(2 * pi * 1000 * static_cast<double>(i) / 48000));
    const auto end = Clock::now() + duration;
    auto next = Clock::now();
    while (Clock::now() < end) {
        source.captureFrame(frame, 1000);
        next += 10ms;
        std::this_thread::sleep_until(next);
    }
}

int contract() {
    // Sources/tracks outlive room teardown, including exception unwinding.
    auto source = std::make_shared<livekit::AudioSource>(48000, 1, 0);
    auto track = livekit::LocalAudioTrack::createLocalAudioTrack(track_name, source);
    Peer receiver, isolated, publisher;
    connect(receiver, "GCP_MEDIA_SUBSCRIBER_TOKEN");
    livekit::AudioStream::Options audio_options;
    audio_options.capacity = 8; // bounded decoded-frame queue
    receiver.room.setOnAudioFrameCallback(publisher_identity, track_name,
        [&](const livekit::AudioFrame& frame) { receiver.seen.frame(frame); }, audio_options);
    connect(isolated, "GCP_MEDIA_ISOLATED_TOKEN");
    connect(publisher, "GCP_MEDIA_PUBLISHER_TOKEN");

    const auto publish = [&]() {
        auto participant = publisher.room.localParticipant().lock();
        require(participant != nullptr, "publisher local participant missing");
        livekit::TrackPublishOptions options;
        options.source = livekit::TrackSource::SOURCE_MICROPHONE;
        participant->publishTrack(track, options);
    };
    track->mute();
    publish();
    pump(*source, 1000ms);
    require(receiver.seen.tone_frames == 0, "muted initial publication leaked the test tone");
    std::cout << "GCP_MEDIA_STAGE:initial_muted_publication" << std::endl;
    track->unmute();
    pump(*source, 3500ms);
    require(receiver.seen.subscriptions > 0 && receiver.seen.tone_frames >= 10, "no decoded synthetic audio received");
    require(receiver.seen.invalid_frames == 0, "decoded PCM format invalid");
    require(isolated.seen.subscriptions == 0, "audio crossed a room boundary");
    std::cout << "GCP_MEDIA_STAGE:decoded_tone_and_room_isolation" << std::endl;

    const auto mute_events = receiver.seen.muted.load();
    track->mute();
    wait_for([&]() { return receiver.seen.muted > mute_events; }, "remote mute event missing");
    pump(*source, 1000ms); // drain in-flight packets and decoder concealment
    const auto muted_frames = receiver.seen.tone_frames.load();
    pump(*source, 1000ms);
    require(receiver.seen.tone_frames == muted_frames, "muted publication still transports the test tone");
    const auto unmute_events = receiver.seen.unmuted.load();
    track->unmute();
    pump(*source, 2000ms);
    require(receiver.seen.unmuted > unmute_events && receiver.seen.tone_frames >= muted_frames + 10,
            "unmuted audio did not resume");
    std::cout << "GCP_MEDIA_STAGE:remote_mute_and_unmute" << std::endl;

    require(publisher.room.disconnect(), "publisher disconnect failed");
    wait_for([&]() { return receiver.seen.departures >= 1; }, "remote departure event missing");
    require(receiver.seen.disconnections == 0, "publisher leave disconnected receiver");
    // Explicit room rejoin, not a simulated SDK network-recovery claim.
    connect(publisher, "GCP_MEDIA_PUBLISHER_TOKEN");
    source = std::make_shared<livekit::AudioSource>(48000, 1, 0);
    track = livekit::LocalAudioTrack::createLocalAudioTrack(track_name, source);
    track->mute();
    publish();
    track->unmute();
    const auto before_rejoin = receiver.seen.tone_frames.load();
    pump(*source, 3000ms);
    require(receiver.seen.tone_frames >= before_rejoin + 10, "audio did not recover after explicit rejoin");
    std::cout << "GCP_MEDIA_STAGE:explicit_rejoin" << std::endl;

    const auto before_eviction = publisher.seen.disconnections.load();
    std::cout << "GCP_MEDIA_READY_FOR_EVICTION" << std::endl;
    // The runner removes this actual active peer through RoomService.
    wait_for([&]() { return publisher.seen.disconnections > before_eviction; }, "active SFU eviction did not disconnect publisher");
    wait_for([&]() { return receiver.seen.departures >= 2; }, "evicted publisher departure missing");
    require(receiver.seen.disconnections == 0 && isolated.seen.subscriptions == 0, "eviction affected unrelated room/peer");
    require(receiver.seen.invalid_frames == 0, "decoded PCM format changed");
    receiver.room.disconnect();
    isolated.room.disconnect();
    std::cout << "GCP_MEDIA_CONTRACT:passed decoded_frames=" << receiver.seen.frames
              << " tone_frames=" << receiver.seen.tone_frames
              << " physical_device_verified=false remote_audio_heard_verified=false" << std::endl;
    return 0;
}
} // namespace

int main() {
    livekit::initialize(livekit::LogLevel::Warn);
    try {
        const int result = contract();
        livekit::shutdown();
        return result;
    } catch (const ContractFailure& error) {
        // Only fixed, local assertion messages are safe to show.
        std::cout << "GCP_MEDIA_CONTRACT:failed " << error.what() << std::endl;
        livekit::shutdown();
        return 1;
    } catch (const std::exception&) {
        // SDK messages could contain credentials; do not print them.
        std::cout << "GCP_MEDIA_CONTRACT:failed" << std::endl;
        livekit::shutdown();
        return 1;
    }
}
