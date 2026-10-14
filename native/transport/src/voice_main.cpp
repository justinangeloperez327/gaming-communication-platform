#include "voice_audio.h"

#include "livekit/livekit.h"
#include "livekit/audio_frame.h"
#include "livekit/audio_source.h"
#include "livekit/room.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr std::string_view kVoiceTrack = "gcp-voice-microphone";
std::atomic<bool> running{true};

void interrupt_handler(int) { running.store(false); }

struct Options {
    std::string mode = "--help";
    std::string sender;
    int seconds = 30;
    int input = -1;
    int output = -1;
    bool confirmed = false;
};
int decimal(std::string_view text) {
    int n = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("Expected an integer device index or duration");
    return n;
}
Options parse(int argc, char* argv[]) {
    Options o;
    if (argc < 2) return o;
    o.mode = argv[1];
    if (o.mode != "--help" && o.mode != "--self-test"
        && o.mode != "--list-devices" && o.mode != "--join"
        && o.mode != "--listen") {
        throw std::invalid_argument("Unknown voice action");
    }
    if (o.mode == "--help" || o.mode == "--self-test" || o.mode == "--list-devices") {
        if (argc != 2) throw std::invalid_argument("Unexpected extra options");
        return o;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (++i >= argc) throw std::invalid_argument("Missing voice argument value");
        const std::string_view value = argv[i];
        if (key == "--sender") o.sender = std::string(value);
        else if (key == "--seconds") o.seconds = decimal(value);
        else if (key == "--input-device") o.input = decimal(value);
        else if (key == "--output-device") o.output = decimal(value);
        else if (key == "--confirm-microphone") {
            if (value != "yes") throw std::invalid_argument("Explicit consent requires yes");
            o.confirmed = true;
        } else throw std::invalid_argument("Unknown voice option");
    }
    if (o.seconds < 1 || o.seconds > 300 || o.input < -1 || o.output < -1)
        throw std::invalid_argument("Invalid voice duration/device index");
    if (o.mode == "--join" && !o.confirmed)
        throw std::invalid_argument("--join requires --confirm-microphone yes");
    if (o.mode == "--listen" && (o.confirmed || o.input != -1 || o.sender.empty()))
        throw std::invalid_argument("--listen requires --sender and never captures a microphone");
    if (o.sender.size() > 128 || o.sender.find_first_of("\r\n\t ") != std::string::npos)
        throw std::invalid_argument("Invalid sender participant identity");
    return o;
}
void usage() {
    std::cout
        << "GCP native Windows LiveKit voice prototype (WASAPI 48k mono PCM16)\n"
        << "  --help | --self-test | --list-devices\n"
        << "  --join --confirm-microphone yes [--sender REMOTE_ID] [--seconds 30]\n"
        << "        [--input-device N] [--output-device N]\n"
        << "  --listen --sender REMOTE_ID [--seconds 30] [--output-device N]\n"
        << "  Device index -1 means the Windows default communications endpoint.\n"
        << "  LIVEKIT_URL and LIVEKIT_TOKEN must be supplied through the environment.\n"
        << "  --join SHARES YOUR LIVE MICROPHONE with other LiveKit room members.\n"
        << "  --listen NEVER opens the microphone. Never use this executable for\n"
        << "  recording; no audio files are created. Ctrl+C to stop.\n"
        << "  Experimental: use a headset to avoid speaker feedback/echo.\n";
}
bool self_test() {
    char app[] = "voice";
    char join[] = "--join";
    char listen[] = "--listen";
    char confirm[] = "--confirm-microphone";
    char yes[] = "yes";
    char sender[] = "--sender";
    char remote[] = "remote-user";
    char* invalid[] = {app, join};
    try { (void)parse(2, invalid); return false; }
    catch (const std::invalid_argument&) {}
    char* join_ok[] = {app, join, confirm, yes, sender, remote};
    auto a = parse(6, join_ok);
    if (!a.confirmed || a.sender != remote || a.mode != join) return false;
    char* listen_ok[] = {app, listen, sender, remote};
    auto b = parse(4, listen_ok);
    if (b.confirmed || b.sender != remote || b.mode != listen) return false;
    char* listen_bad[] = {app, listen, sender, remote, confirm, yes};
    try { (void)parse(6, listen_bad); return false; }
    catch (const std::invalid_argument&) {}
    return gcp::voice::queue_self_test();
}
struct SdkSession {
    SdkSession() { livekit::initialize(livekit::LogLevel::Warn); }
    ~SdkSession() { livekit::shutdown(); }
    SdkSession(const SdkSession&) = delete;
    SdkSession& operator=(const SdkSession&) = delete;
};

std::string endpoint() {
    const auto* value = std::getenv("LIVEKIT_URL");
    if (!value || !*value) throw std::invalid_argument("LIVEKIT_URL is required");
    const std::string url(value);
    if (!url.starts_with("wss://") && !url.starts_with("ws://localhost:")
        && !url.starts_with("ws://127.0.0.1:"))
        throw std::invalid_argument("LiveKit URL must be wss:// (or loopback ws://)");
    return url;
}
std::string credential() {
    const auto* value = std::getenv("LIVEKIT_TOKEN");
    if (!value || !*value) throw std::invalid_argument("LIVEKIT_TOKEN is required");
    return value;
}
void receive_audio(const livekit::AudioFrame& frame,
                   gcp::voice::PlaybackQueue& playback,
                   gcp::voice::Counters& stats) {
    const int in_rate = frame.sampleRate();
    const int channels = frame.numChannels();
    const int frames = frame.samplesPerChannel();
    // Bound sample count and reject unsupported/deceptive SDK metadata.
    if (in_rate < 16000 || in_rate > 96000 || (channels != 1 && channels != 2)
        || frames <= 0 || frames > 9600
        || frame.data().size() != static_cast<std::size_t>(frames * channels)) {
        ++stats.dropped;
        return;
    }
    const auto output_count = static_cast<std::size_t>(
        (static_cast<std::int64_t>(frames) * gcp::voice::kRate) / in_rate);
    if (output_count == 0 || output_count > gcp::voice::kMaxBufferedSamples) {
        ++stats.dropped;
        return;
    }
    std::vector<std::int16_t> normalized(output_count);
    for (std::size_t i = 0; i < output_count; ++i) {
        // Bounded nearest-neighbor format adaptation; replace with quality
        // resampler and AEC/AGC for product audio in later groups.
        const auto src = std::min(static_cast<std::size_t>(frames - 1),
            i * static_cast<std::size_t>(in_rate) / gcp::voice::kRate);
        const auto at = src * static_cast<std::size_t>(channels);
        const int value = channels == 2
            ? (static_cast<int>(frame.data()[at]) +
               static_cast<int>(frame.data()[at + 1])) / 2
            : frame.data()[at];
        normalized[i] = static_cast<std::int16_t>(value);
    }
    stats.overwritten += playback.push(normalized);
    ++stats.decoded;
}

int run(const Options& opts) {
    const auto url = endpoint();
    const auto token = credential();
    SdkSession sdk;

    auto room = std::make_unique<livekit::Room>();
    auto playback = std::make_shared<gcp::voice::PlaybackQueue>();
    auto stats = std::make_shared<gcp::voice::Counters>();
    if (!opts.sender.empty()) {
        // Register BEFORE connecting. The LiveKit SDK starts the reader at
        // track-subscription time; late callbacks may miss that lifecycle.
        room->setOnAudioFrameCallback(opts.sender, std::string(kVoiceTrack),
            [playback, stats](const livekit::AudioFrame& audio) {
                receive_audio(audio, *playback, *stats);
            });
    }
    livekit::RoomOptions options;
    options.auto_subscribe = true;
    options.connect_timeout = 15s;
    if (!room->connect(url, token, options))
        throw std::runtime_error("LiveKit voice room connection failed");
    std::shared_ptr<livekit::AudioSource> source;
    std::shared_ptr<livekit::LocalAudioTrack> track;
    if (opts.mode == "--join") {
        auto local = room->localParticipant().lock();
        if (!local) throw std::runtime_error("Local LiveKit identity not found");
        source = std::make_shared<livekit::AudioSource>(
            gcp::voice::kRate, gcp::voice::kChannels, 100);
        track = local->publishAudioTrack(
            std::string(kVoiceTrack), source, livekit::TrackSource::SOURCE_MICROPHONE);
        if (!track) throw std::runtime_error("Unable to publish microphone track");
        std::cerr << "MICROPHONE SHARING ACTIVE: explicit user consent. Ctrl+C to stop.\n";
    }

    std::exception_ptr audio_error;
    std::thread speaker;
    std::thread microphone;
     // At most one audio thread per action; no concurrent writes to the error
    // variable because each thread guards it through a small mutex.
    std::mutex errors;
    auto record_error = [&](const std::function<void()>& action) {
        try { action(); }
        catch (...) {
            std::lock_guard lock(errors);
            if (!audio_error) audio_error = std::current_exception();
            running.store(false);
        }
    };
    try {
        if (!opts.sender.empty()) {
            speaker = std::thread([&] {
                record_error([&] {
                    gcp::voice::play_speakers(opts.output, running, *playback, *stats);
                });
            });
        }
        if (source) {
            microphone = std::thread([&] {
                record_error([&] {
                    gcp::voice::capture_microphone(opts.input, running, *stats,
                        [&](std::span<const std::int16_t> block) {
                            std::vector<std::int16_t> pcm(block.begin(), block.end());
                            const livekit::AudioFrame frame(
                                std::move(pcm), gcp::voice::kRate,
                                gcp::voice::kChannels, gcp::voice::kBlockFrames);
                            source->captureFrame(frame, 20);
                        });
                });
            });
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(opts.seconds);
        while (running.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
    } catch (...) {
        running.store(false);
        if (microphone.joinable()) microphone.join();
        if (speaker.joinable()) speaker.join();
        room->disconnect();
        throw;
    }
    running.store(false);
    if (microphone.joinable()) microphone.join();
    if (speaker.joinable()) speaker.join();

    if (!opts.sender.empty()) room->clearOnAudioFrameCallback(
        opts.sender, std::string(kVoiceTrack));
    room->disconnect();
    // No raw samples, tokens, real participant IDs or private room names
    // are written to stdout.
    std::cout << "{\"probe\":\"livekit_voice_wasapi\",\"captured_samples\":"
        << stats->captured.load() << ",\"published_frames\":" << stats->published.load()
        << ",\"rendered_samples\":" << stats->played.load()
        << ",\"decoded_remote_frames\":" << stats->decoded.load()
        << ",\"overflow_discarded_samples\":" << stats->overwritten.load()
        << ",\"underflow_silence_samples\":" << stats->silence.load()
        << ",\"invalid_remote_frames\":" << stats->dropped.load() << "}\n";
    if (audio_error) std::rethrow_exception(audio_error);
    const bool local_ok = opts.mode != "--join" || stats->published.load() > 0;
    const bool remote_ok = opts.sender.empty() || stats->decoded.load() > 0;
    return local_ok && remote_ok ? 0 : 2;
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, interrupt_handler);
        const auto opts = parse(argc, argv);
        if (opts.mode == "--help") { usage(); return 0; }
        if (opts.mode == "--self-test") {
            if (!self_test()) return 1;
            std::cout << "Voice CLI and bounded PCM playback queue self-test passed\n";
            return 0;
        }
        if (opts.mode == "--list-devices") {
            gcp::voice::list_devices();
            return 0;
        }
        return run(opts);
    } catch (const std::exception& ex) {
        std::cerr << "Voice diagnostic failed: " << ex.what() << "\n";
        return 1;
    }
}
