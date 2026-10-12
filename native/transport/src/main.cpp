#include "telemetry.h"
#include "screen_capture.h"
#include "annexb_h264.h"
#include "livekit/encoded_video_source.h"

#include "livekit/livekit.h"
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kTrackName = "gcp-test-video";
constexpr std::string_view kScreenTrackName = "gcp-screen-share";
constexpr std::string_view kEncodedTrackName = "gcp-h264-encoded";
std::atomic<bool> g_running{true};

void interrupt_handler(int) {
    g_running.store(false);
}

struct Options {
    std::string action;
    std::string sender;
    std::string file;
    bool confirm_video_file_share = false;
    std::string track = "test";
    int monitor = -1;
    bool confirm_screen_share = false;
    int seconds = 15;
    int fps = 30;
    int width = 640;
    int height = 360;
};

bool number(std::string_view s, int& value) {
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    return error == std::errc{} && end == s.data() + s.size();
}

Options parse(int argc, char* argv[]) {
    Options options;
    if (argc < 2) {
        options.action = "help";
        return options;
    }
    options.action = argv[1];
    if (options.action != "publish" && options.action != "watch" &&
        options.action != "share-monitor" &&
        options.action != "publish-h264" &&
        options.action != "--self-test" && options.action != "--help") {
        throw std::invalid_argument("Unknown action");
    }

    if (options.action == "--help" || options.action == "--self-test") {
        if (argc != 2) throw std::invalid_argument("Unexpected extra arguments");
        return options;
    }

    for (int i = 2; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--confirm-screen-share") {
            options.confirm_screen_share = true;
            continue;
        }
        if (name == "--confirm-video-file-share") {
            options.confirm_video_file_share = true;
            continue;
        }
        if (++i == argc) throw std::invalid_argument("Missing argument value");
        const std::string_view value = argv[i];
        if (name == "--sender") options.sender = std::string(value);
        else if (name == "--track") options.track = std::string(value);
        else if (name == "--file") options.file = std::string(value);
        else if (name == "--monitor") {
            if (!number(value, options.monitor) || options.monitor < 0)
                throw std::invalid_argument("--monitor requires a nonnegative index");
        }
        else if (name == "--seconds") {
            if (!number(value, options.seconds) ||
                options.seconds < 1 || options.seconds > 300)
                throw std::invalid_argument("--seconds must be 1..300");
        } else if (name == "--fps") {
            if (!number(value, options.fps) ||
                options.fps < 1 || options.fps > 60)
                throw std::invalid_argument("--fps must be 1..60");
        } else if (name == "--width") {
            if (!number(value, options.width) ||
                options.width < 160 || options.width > 1920 || options.width % 2 != 0)
                throw std::invalid_argument("--width must be even and 160..1920");
        } else if (name == "--height") {
            if (!number(value, options.height) ||
                options.height < 90 || options.height > 1080 || options.height % 2 != 0)
                throw std::invalid_argument("--height must be even and 90..1080");
        } else {
            throw std::invalid_argument("Unknown option");
        }
    }
    if (options.action == "watch" && options.sender.empty()) {
        throw std::invalid_argument("watch requires --sender <participant-identity>");
    }
    if (options.action == "watch" &&
        (options.fps != 30 || options.width != 640 || options.height != 360)) {
        throw std::invalid_argument("Video settings only apply to publish");
    }
    if (options.action != "watch" &&
        (!options.sender.empty() || options.track != "test"))
        throw std::invalid_argument("--sender/--track only apply to watch");
    if (options.action == "watch" &&
        options.track != "test" && options.track != "screen" && options.track != "h264")
        throw std::invalid_argument("--track must be test, screen or h264");
    if (options.action != "share-monitor" &&
        (options.confirm_screen_share || options.monitor != -1))
        throw std::invalid_argument("--monitor/confirmation only apply to share-monitor");
    if (options.action == "publish-h264") {
        if (options.file.empty() || !options.confirm_video_file_share)
            throw std::invalid_argument(
                "publish-h264 needs --file PATH and --confirm-video-file-share");
    } else if (!options.file.empty() || options.confirm_video_file_share) {
        throw std::invalid_argument(
            "--file and confirmation are only valid for publish-h264");
    }
    if (options.action == "share-monitor") {
        if (options.monitor < 0 || !options.confirm_screen_share)
            throw std::invalid_argument(
                "share-monitor requires --monitor N and --confirm-screen-share");
        if (options.width != 640 || options.height != 360)
            throw std::invalid_argument(
                "share-monitor uses native size, not --width/--height");
    }
    return options;
}

void usage() {
    std::cout <<
        "GCP LiveKit native media transport feasibility tests\n"
        "  --help\n"
        "  --self-test\n"
        "  publish [--seconds 15] [--fps 30] [--width 640] [--height 360]\n"
        "  watch --sender ID [--track test|screen|h264] [--seconds 15]\n"
        "  share-monitor --monitor N --confirm-screen-share [--fps 30] [--seconds 15]\n"
        "  publish-h264 --file PATH --confirm-video-file-share --width W --height H\n"
        "    [--fps 30] [--seconds 15]; reads a pre-encoded Annex B H.264 file.\n"
        "Requires LIVEKIT_URL and LIVEKIT_TOKEN in the environment for both modes.\n"
        "share-monitor CAPTURES the selected display and transmits to the LiveKit room.\n"
        "Explicit opt-in is mandatory. No microphone is captured. Tokens are never printed.\n"
        "Monitor capture uses CPU readback; publish-h264 does NOT encode frames.\n"
        "The confirmed H.264 file is transmitted to the LiveKit room.\n";
}

int self_test() {
    char app[] = "test";
    char mode[] = "publish";
    char flag[] = "--fps";
    char rate[] = "60";
    char* valid[] = {app, mode, flag, rate};
    const auto options = parse(4, valid);
    if (options.action != "publish" || options.fps != 60) return 1;
    int parsed = 0;
    if (!number("30", parsed) || parsed != 30 || number("30garbage", parsed)) return 1;
    try {
        char bad_mode[] = "watch";
        char* invalid[] = {app, bad_mode};
        (void)parse(2, invalid);
        return 1;
    } catch (const std::invalid_argument&) {
        // A remote identity must be supplied for watch mode.
    }
    try {
        char action[] = "share-monitor";
        char monitor_flag[] = "--monitor";
        char monitor_index[] = "0";
        char* invalid_share[] = {app, action, monitor_flag, monitor_index};
        (void)parse(4, invalid_share);
        return 1;
    } catch (const std::invalid_argument&) {
        // Consent cannot be inferred from the presence of a monitor index.
    }
    char action[] = "share-monitor";
    char monitor_flag[] = "--monitor";
    char monitor_index[] = "0";
    char consent[] = "--confirm-screen-share";
    char* valid_share[] = {app, action, monitor_flag, monitor_index, consent};
    auto selected = parse(5, valid_share);
    if (selected.monitor != 0 || !selected.confirm_screen_share) return 1;
    if (!gcp::transport::test_annexb_h264_parser()) return 1;
    try {
        char encoded_action[] = "publish-h264";
        char file_flag[] = "--file";
        char fake_path[] = "test.h264";
        char* missing_consent[] = {app, encoded_action, file_flag, fake_path};
        (void)parse(4, missing_consent);
        return 1;
    } catch (const std::invalid_argument&) {
    }
    livekit::initialize(livekit::LogLevel::Warn);
    livekit::shutdown();
    std::cout << "LiveKit SDK linked and CLI self-test passed\n";
    return 0;
}

void synthetic_frame(livekit::VideoFrame& frame, std::uint64_t index) {
    // Fixed RGBA test pattern with one moving band. Intentionally CPU-backed:
    // this test validates transport, NOT zero-copy capture or hardware encoding.
    auto* pixels = frame.data();
    const auto width = frame.width();
    const auto height = frame.height();
    const auto band = static_cast<int>((index * 6u) % static_cast<std::uint64_t>(width));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y * width + x) * 4u;
            const auto bright = x >= band && x < band + 50;
            pixels[offset] = bright ? 235 : static_cast<std::uint8_t>(x * 180 / width);
            pixels[offset + 1] = static_cast<std::uint8_t>(y * 160 / height);
            pixels[offset + 2] = bright ? 40 : 120;
            pixels[offset + 3] = 255;
        }
    }
}

int publish(livekit::Room& room, const Options& options) {
    auto participant = room.localParticipant().lock();
    if (!participant) throw std::runtime_error("No local participant");
    auto source = std::make_shared<livekit::VideoSource>(options.width, options.height);
    auto track = participant->publishVideoTrack(
        std::string(kTrackName), source, livekit::TrackSource::SOURCE_CAMERA);
    if (!track) throw std::runtime_error("Unable to publish synthetic video");
    std::cout << "Publishing synthetic video (no screen capture)\n";

    const auto start = Clock::now();
    const auto before = gcp::probe::take_snapshot();
    const auto frame_period = std::chrono::nanoseconds(1'000'000'000LL / options.fps);
    auto due = start;
    std::uint64_t frames = 0;

    while (g_running.load() &&
           Clock::now() - start < std::chrono::seconds(options.seconds)) {
        auto frame = livekit::VideoFrame::create(
            options.width, options.height, livekit::VideoBufferType::RGBA);
        synthetic_frame(frame, frames);
        source->captureFrame(frame);
        ++frames;

        // Avoid catch-up bursts if the encoder runs behind schedule.
        due += frame_period;
        const auto now = Clock::now();
        if (due < now) due = now;
        std::this_thread::sleep_until(due);
    }
    const auto after = gcp::probe::take_snapshot();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    gcp::probe::print_metrics("livekit_synthetic_publish", elapsed, frames, 0,
                             before, after, options.width, options.height);
    return frames > 0 ? 0 : 2;
}

int share_monitor(livekit::Room& room, const Options& options) {
    // The capture constructor rejects invalid/oversized monitors before
    // publishing a track. This path only runs after explicit CLI consent.
    gcp::transport::ScreenCapture capture(options.monitor, options.fps);
    auto participant = room.localParticipant().lock();
    if (!participant) throw std::runtime_error("No local participant");
    auto source = std::make_shared<livekit::VideoSource>(
        capture.width(), capture.height());
    auto track = participant->publishVideoTrack(
        std::string(kScreenTrackName), source,
        livekit::TrackSource::SOURCE_SCREENSHARE);
    if (!track) throw std::runtime_error("Could not publish screen-share track");

    std::cerr << "SCREEN SHARING ACTIVE: monitor " << options.monitor
              << ". Press Ctrl+C or terminate the process to stop.\n";
    const auto start = Clock::now();
    const auto before = gcp::probe::take_snapshot();
    capture.start();
    std::uint64_t sequence = 0;
    std::uint64_t published = 0;
    gcp::transport::ScreenFrame sample;
    while (g_running.load() &&
           Clock::now() - start < std::chrono::seconds(options.seconds)) {
        if (!capture.take_latest(sequence, sample)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (sample.bgra.size() !=
            static_cast<std::size_t>(capture.width()) *
                static_cast<std::size_t>(capture.height()) * 4) {
            throw std::runtime_error("Invalid captured BGRA buffer");
        }
        auto frame = livekit::VideoFrame::create(
            capture.width(), capture.height(), livekit::VideoBufferType::BGRA);
        std::memcpy(frame.data(), sample.bgra.data(), sample.bgra.size());
        source->captureFrame(frame);
        ++published;
    }
    const auto stats = capture.stats();
    const auto after = gcp::probe::take_snapshot();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    gcp::probe::print_metrics(
        "livekit_monitor_publish", elapsed, published, stats.copied,
        before, after, capture.width(), capture.height());
    std::cout << "{\"capture_arrivals\":" << stats.arrivals
              << ",\"readbacks\":" << stats.copied
              << ",\"throttled\":" << stats.throttled
              << ",\"rejected\":" << stats.rejected
              << ",\"failures\":" << stats.failures
              << ",\"readback_p50_ms\":"
              << (static_cast<double>(stats.readback_p50_microseconds) / 1000.0)
              << ",\"readback_p95_ms\":"
              << (static_cast<double>(stats.readback_p95_microseconds) / 1000.0)
              << ",\"mean_gpu_readback_ms\":"
              << (stats.copied == 0 ? 0.0 :
                  static_cast<double>(stats.readback_microseconds) /
                  (1000.0 * static_cast<double>(stats.copied)))
              << ",\"offered_frames\":" << published << "}\n";
    if (stats.failures || stats.rejected) return 3;
    return published > 0 ? 0 : 2;
}


int publish_h264_file(livekit::Room& room, const Options& options) {
    namespace fs = std::filesystem;
    const fs::path path(options.file);
    if (!fs::is_regular_file(path)) {
        throw std::invalid_argument("H.264 test file does not exist");
    }
    const auto size = fs::file_size(path);
    if (size == 0 || size > 64u * 1024u * 1024u) {
        throw std::invalid_argument("H.264 file must be 1..64 MiB");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open encoded file");
    std::vector<std::uint8_t> encoded{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("Unable to read H.264 test file");
    }
    const auto units = gcp::transport::split_annexb_access_units(encoded);

    auto participant = room.localParticipant().lock();
    if (!participant) throw std::runtime_error("No local participant");
    auto source = std::make_shared<livekit::EncodedVideoSource>(
        livekit::VideoCodec::H264, options.width, options.height);
    auto track = participant->publishVideoTrack(
        std::string(kEncodedTrackName), source,
        livekit::TrackSource::SOURCE_SCREENSHARE);
    if (!track) throw std::runtime_error("Encoded H.264 track publish failed");

    std::cerr << "PRE-ENCODED H.264 SHARING ACTIVE: explicitly selected file. "
              << "No hardware encoder is invoked in this process.\n";
    const auto started = Clock::now();
    const auto before = gcp::probe::take_snapshot();
    const auto step = std::chrono::nanoseconds(1'000'000'000LL / options.fps);
    std::uint64_t accepted = 0, declined = 0, keyframe_requests = 0;
    for (std::size_t i = 0; i < units.size() && g_running.load(); ++i) {
        const auto due = started + step * static_cast<std::int64_t>(i);
        if (due - started >= std::chrono::seconds(options.seconds)) break;
        std::this_thread::sleep_until(due);
        const auto feedback = source->takeFeedback();
        if (feedback.keyframe_requested) {
            // Static files cannot generate a new IDR on demand. This is an
            // explicit limitation, not a replacement for encoder feedback.
            ++keyframe_requests;
        }
        livekit::EncodedVideoSource::Frame frame;
        frame.is_keyframe = units[i].keyframe;
        frame.timestamp_us = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(i) * 1'000'000u) /
            static_cast<std::uint64_t>(options.fps));
        frame.data = units[i].bytes;
        if (source->captureFrame(frame)) ++accepted;
        else ++declined;
    }
    const auto after = gcp::probe::take_snapshot();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    gcp::probe::print_metrics(
        "livekit_preencoded_h264", elapsed, accepted, declined,
        before, after, options.width, options.height);
    std::cout << "{\"access_units\":" << units.size()
              << ",\"accepted\":" << accepted
              << ",\"rejected\":" << declined
              << ",\"unfulfilled_keyframe_requests\":" << keyframe_requests
              << ",\"encoder\":\"external_preencoded_file\"}\n";
    return accepted > 0 && declined == 0 ? 0 : 2;
}

int watch(livekit::Room& room, const Options& options) {
    struct Received {
        std::atomic<std::uint64_t> frames{0};
        std::atomic<int> width{0};
        std::atomic<int> height{0};
    };
    // Shared state remains valid even if the SDK finishes an in-flight
    // callback after the subscription is cleared during shutdown.
    auto received = std::make_shared<Received>();
    const auto track_name = options.track == "screen"
        ? std::string(kScreenTrackName)
        : options.track == "h264"
            ? std::string(kEncodedTrackName)
            : std::string(kTrackName);
    room.setOnVideoFrameCallback(options.sender, track_name,
        [received](const livekit::VideoFrame& frame, std::int64_t) {
            received->width.store(frame.width());
            received->height.store(frame.height());
            received->frames.fetch_add(1);
        });

    std::cout << "Receiving synthetic test track from selected participant\n";
    const auto start = Clock::now();
    const auto before = gcp::probe::take_snapshot();
    while (g_running.load() &&
           Clock::now() - start < std::chrono::seconds(options.seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Stop callbacks before local metrics objects are destroyed.
    room.clearOnVideoFrameCallback(options.sender, track_name);
    const auto after = gcp::probe::take_snapshot();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto count = received->frames.load();
    gcp::probe::print_metrics("livekit_synthetic_receive", elapsed, count, 0,
                             before, after, received->width.load(), received->height.load());
    if (count == 0) {
        std::cerr << "No synthetic video received: check identity, track, and room\n";
        return 2;
    }
    return 0;
}

int run(const Options& options) {
    const auto* url = std::getenv("LIVEKIT_URL");
    const auto* token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !*url || !token || !*token)
        throw std::invalid_argument("Set LIVEKIT_URL and LIVEKIT_TOKEN first");

    const std::string_view endpoint(url);
    if (!endpoint.starts_with("wss://") &&
        !endpoint.starts_with("ws://localhost:") &&
        !endpoint.starts_with("ws://127.0.0.1:")) {
        throw std::invalid_argument(
            "Use wss:// for remote hosts (ws:// allowed for local dev only)");
    }

    if (options.action == "share-monitor") {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    livekit::initialize(livekit::LogLevel::Warn);
    std::unique_ptr<livekit::Room> room;
    try {
        int result = 1;
        {
            room = std::make_unique<livekit::Room>();
            livekit::RoomOptions room_options;
            room_options.auto_subscribe = true;
            room_options.connect_timeout = std::chrono::seconds(15);
            if (!room->connect(url, token, room_options))
                throw std::runtime_error("LiveKit room connection failed");
            if (options.action == "publish") result = publish(*room, options);
            else if (options.action == "share-monitor") result = share_monitor(*room, options);
            else if (options.action == "publish-h264") result = publish_h264_file(*room, options);
            else result = watch(*room, options);
            room->disconnect();
            room.reset();
        }
        livekit::shutdown();
        return result;
    } catch (...) {
        room.reset();  // Dispose room before shutting down the SDK.
        livekit::shutdown();
        throw;
    }
}
}

int main(int argc, char* argv[]) {
    try {
        const auto options = parse(argc, argv);
        if (options.action == "--help" || options.action == "help") {
            usage();
            return 0;
        }
        if (options.action == "--self-test") return self_test();
        std::signal(SIGINT, interrupt_handler);
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "LiveKit transport smoke test failed: " << error.what() << "\n";
        return 1;
    }
}
