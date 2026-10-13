#include "hardware_h264_bridge.h"
#include "h264_access_unit.h"
#include "livekit/livekit.h"
#include "livekit/encoded_video_source.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
std::atomic<bool> interrupted{false};
void interrupt_handler(int) { interrupted.store(true); }

struct Options {
    bool publish = false;
    gcp::media::HardwareH264Profile profile;
};

bool number(std::string_view value, int& out) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    return error == std::errc{} && end == value.data() + value.size();
}

Options parse(int argc, char* argv[]) {
    Options result;
    if (argc < 2 || (argc == 2 && std::string_view(argv[1]) == "--help"))
        return result;
    if (std::string_view(argv[1]) != "--publish-monitor")
        throw std::invalid_argument("Expected --publish-monitor");
    result.publish = true;
    auto& p = result.profile;
    bool consent = false;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc)
            throw std::invalid_argument("Missing option value");
        const std::string_view key(argv[i]), value(argv[i + 1]);
        if (key == "--confirm-screen-share") {
            if (value != "yes")
                throw std::invalid_argument("Screen-sharing confirmation must be yes");
            consent = true;
            continue;
        }
        int n = 0;
        if (!number(value, n))
            throw std::invalid_argument("Numeric argument expected");
        if (key == "--monitor") p.monitor_index = n;
        else if (key == "--width") p.width = n;
        else if (key == "--height") p.height = n;
        else if (key == "--fps") p.fps = n;
        else if (key == "--frames") p.frames = n;
        else if (key == "--bitrate") p.bitrate_bps = n;
        else if (key == "--timeout") p.timeout_seconds = n;
        else throw std::invalid_argument("Unknown command argument");
    }
    p.monitor_capture_confirmed = consent;
    if (p.monitor_index < 0 || !consent ||
        p.width < 160 || p.width > 1920 || p.width % 2 ||
        p.height < 90 || p.height > 1080 || p.height % 2 ||
        p.fps < 1 || p.fps > 60 || p.frames < 1 || p.frames > 60 ||
        p.bitrate_bps < 100'000 || p.bitrate_bps > 16'000'000 ||
        p.timeout_seconds < 5 || p.timeout_seconds > 120)
        throw std::invalid_argument(
            "Choose monitor, explicit sharing consent, and valid bounded H.264 profile");
    return result;
}

void usage() {
    std::cout <<
        "GCP Hardware H.264 → LiveKit diagnostic\n"
        "  --help | --self-test\n"
        "  --publish-monitor --monitor N --confirm-screen-share yes\n"
        "     [--width 1920] [--height 1080] [--fps 30] [--frames 30]\n"
        "     [--bitrate 4000000] [--timeout 30]\n"
        "SHARES THE ENTIRE SELECTED MONITOR with LiveKit room participants.\n"
        "Requires LIVEKIT_URL, LIVEKIT_TOKEN and compatible hardware H.264 MFT.\n"
        "No audio or disk recording. Bounded to 60 submitted input frames.\n";
}

int self_test() {
    if (!gcp::transport::h264_access_unit_self_test()) return 1;
    char app[] = "app";
    char publish[] = "--publish-monitor";
    char* missing_consent[] = {app, publish};
    try { (void)parse(2, missing_consent); return 1; }
    catch (const std::invalid_argument&) {}
    char monitor[] = "--monitor", zero[] = "0";
    char consent[] = "--confirm-screen-share", yes[] = "yes";
    char* good[] = {app, publish, monitor, zero, consent, yes};
    const auto options = parse(6, good);
    if (!options.publish || options.profile.monitor_index != 0 ||
        !options.profile.monitor_capture_confirmed) return 1;
    char denied[] = "no";
    char* bad[] = {app, publish, monitor, zero, consent, denied};
    try { (void)parse(6, bad); return 1; }
    catch (const std::invalid_argument&) {}
    livekit::initialize(livekit::LogLevel::Warn);
    livekit::shutdown();
    std::cout << "H.264 access-unit and consent self-test passed\n";
    return 0;
}

int publish(const Options& options) {
    const auto* endpoint = std::getenv("LIVEKIT_URL");
    const auto* token = std::getenv("LIVEKIT_TOKEN");
    if (!endpoint || !*endpoint || !token || !*token)
        throw std::invalid_argument("Set LIVEKIT_URL and LIVEKIT_TOKEN");
    const std::string_view endpoint_view(endpoint);
    if (!endpoint_view.starts_with("wss://") &&
        !endpoint_view.starts_with("ws://localhost:") &&
        !endpoint_view.starts_with("ws://127.0.0.1:"))
        throw std::invalid_argument("Use wss:// for non-loopback LiveKit servers");

    livekit::initialize(livekit::LogLevel::Warn);
    std::unique_ptr<livekit::Room> room;
    try {
        room = std::make_unique<livekit::Room>();
        livekit::RoomOptions room_options;
        room_options.auto_subscribe = false;
        room_options.connect_timeout = std::chrono::seconds(15);
        if (!room->connect(endpoint, token, room_options))
            throw std::runtime_error("LiveKit room connection failed");
        auto participant = room->localParticipant().lock();
        if (!participant)
            throw std::runtime_error("Missing local LiveKit participant");

        const auto& p = options.profile;
        auto source = std::make_shared<livekit::EncodedVideoSource>(
            livekit::VideoCodec::H264, p.width, p.height);
        auto track = participant->publishVideoTrack(
            "gcp-h264-encoded", source, livekit::TrackSource::SOURCE_SCREENSHARE);
        if (!track) throw std::runtime_error("H.264 screen track publication failed");

        std::uint64_t accepted = 0, rejected = 0, skipped = 0;
        std::uint64_t unhandled_keyframes = 0, unhandled_rate = 0;
        std::int64_t last_ts = -1;
        bool first_idr_sent = false;
        const gcp::media::HardwareH264Callback emit =
            [&](const gcp::media::HardwareH264AccessUnit& encoded) -> bool {
                if (interrupted.load()) return false;
                auto verified = gcp::transport::verify_h264_access_unit(encoded.bytes);
                if (encoded.keyframe && !verified.keyframe)
                    throw std::runtime_error("Encoder's keyframe metadata is inconsistent");
                if (encoded.timestamp_us < 0 || encoded.timestamp_us < last_ts)
                    throw std::runtime_error("Invalid H.264 output timestamp");
                last_ts = encoded.timestamp_us;

                const auto feedback = source->takeFeedback();
                if (feedback.keyframe_requested) ++unhandled_keyframes;
                if (feedback.rate_control.has_value()) ++unhandled_rate;
                if (!first_idr_sent && !verified.keyframe) {
                    ++skipped;
                    return true;
                }
                if (verified.keyframe) first_idr_sent = true;

                livekit::EncodedVideoSource::Frame frame;
                frame.is_keyframe = verified.keyframe;
                frame.timestamp_us = encoded.timestamp_us;
                frame.data = std::move(verified.annexb);
                if (!source->captureFrame(frame)) {
                    ++rejected;
                    return false;
                }
                ++accepted;
                return true;
            };

        std::cerr << "SCREEN SHARING ACTIVE: monitor " << p.monitor_index
                  << " is transmitted to the LiveKit room. Ctrl+C to stop.\n";
        const auto result = gcp::media::encode_monitor_h264(p, emit);
        std::cout << "{\"probe\":\"hardware_h264_livekit\""
                  << ",\"packets_accepted\":" << accepted
                  << ",\"packets_rejected\":" << rejected
                  << ",\"pre_idr_skipped\":" << skipped
                  << ",\"unhandled_keyframe_feedback\":" << unhandled_keyframes
                  << ",\"unhandled_rate_feedback\":" << unhandled_rate
                  << ",\"gpu_encoder_engine_verified\":false"
                  << ",\"receiver_playback_verified\":false}\n";
        room->disconnect();
        room.reset();
        livekit::shutdown();
        return result == 0 && accepted && first_idr_sent ? 0 : 2;
    } catch (...) {
        room.reset();
        livekit::shutdown();
        throw;
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
            return self_test();
        const auto options = parse(argc, argv);
        if (!options.publish) {
            usage();
            return 0;
        }
        std::signal(SIGINT, interrupt_handler);
        return publish(options);
    } catch (const std::exception& ex) {
        std::cerr << "Hardware H.264 LiveKit diagnostic failed: " << ex.what() << "\n";
        return 1;
    }
}
