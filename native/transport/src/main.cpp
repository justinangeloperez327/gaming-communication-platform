#include "telemetry.h"

#include "livekit/livekit.h"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kTrackName = "gcp-test-video";
std::atomic<bool> g_running{true};

void interrupt_handler(int) {
    g_running.store(false);
}

struct Options {
    std::string action;
    std::string sender;
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
        options.action != "--self-test" && options.action != "--help") {
        throw std::invalid_argument("Unknown action");
    }

    if (options.action == "--help" || options.action == "--self-test") {
        if (argc != 2) throw std::invalid_argument("Unexpected extra arguments");
        return options;
    }

    for (int i = 2; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (++i == argc) throw std::invalid_argument("Missing argument value");
        const std::string_view value = argv[i];
        if (name == "--sender") options.sender = std::string(value);
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
    if (options.action == "publish" && !options.sender.empty())
        throw std::invalid_argument("--sender only applies to watch");
    return options;
}

void usage() {
    std::cout <<
        "GCP LiveKit native transport feasibility test (synthetic video only)\n"
        "  --help\n"
        "  --self-test\n"
        "  publish [--seconds 15] [--fps 30] [--width 640] [--height 360]\n"
        "  watch --sender ID [--seconds 15]\n"
        "Requires LIVEKIT_URL and LIVEKIT_TOKEN in the environment for both modes.\n"
        "No screen or microphone is accessed. No credentials are printed.\n";
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

int watch(livekit::Room& room, const Options& options) {
    struct Received {
        std::atomic<std::uint64_t> frames{0};
        std::atomic<int> width{0};
        std::atomic<int> height{0};
    };
    // Shared state remains valid even if the SDK finishes an in-flight
    // callback after the subscription is cleared during shutdown.
    auto received = std::make_shared<Received>();
    room.setOnVideoFrameCallback(options.sender, std::string(kTrackName),
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
    room.clearOnVideoFrameCallback(options.sender, std::string(kTrackName));
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

    livekit::initialize(livekit::LogLevel::Warn);
    try {
        int result = 1;
        {
            auto room = std::make_unique<livekit::Room>();
            livekit::RoomOptions room_options;
            room_options.auto_subscribe = true;
            room_options.connect_timeout = std::chrono::seconds(15);
            if (!room->connect(url, token, room_options))
                throw std::runtime_error("LiveKit room connection failed");
            result = options.action == "publish"
                ? publish(*room, options)
                : watch(*room, options);
            room->disconnect();
        }
        livekit::shutdown();
        return result;
    } catch (...) {
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
