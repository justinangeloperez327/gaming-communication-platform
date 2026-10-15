#include "hardware_h264_bridge.h"
#include "h264_access_unit.h"
#include "frame_arrival_metrics.h"
#include "../../probe/src/telemetry.h"
#include "livekit/livekit.h"
#include "livekit/encoded_video_source.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <thread>
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
    int linger_ms = 1000;
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
    bool frames_explicit = false, timeout_explicit = false;
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
        else if (key == "--frames") {
            p.frames = n;
            frames_explicit = true;
        }
        else if (key == "--bitrate") p.bitrate_bps = n;
        else if (key == "--timeout") {
            p.timeout_seconds = n;
            timeout_explicit = true;
        }
        else if (key == "--duration") p.duration_seconds = n;
        else if (key == "--linger-ms") result.linger_ms = n;
        else throw std::invalid_argument("Unknown command argument");
    }
    p.monitor_capture_confirmed = consent;
    // Validate before multiplication/addition: CLI integers may be INT_MAX.
    if (p.fps < 1 || p.fps > 60 ||
        p.duration_seconds < 0 || p.duration_seconds > 300)
        throw std::invalid_argument("Invalid FPS or duration");
    if (p.duration_seconds > 0 && p.duration_seconds <= 300) {
        if (!frames_explicit) p.frames = p.fps * p.duration_seconds;
        if (!timeout_explicit) p.timeout_seconds = p.duration_seconds + 15;
    }
    if (p.monitor_index < 0 || !consent ||
        p.width < 160 || p.width > 1920 || p.width % 2 ||
        p.height < 90 || p.height > 1080 || p.height % 2 ||
        p.fps < 1 || p.fps > 60 || p.frames < 1 || p.frames > 18000 ||
        p.bitrate_bps < 100'000 || p.bitrate_bps > 16'000'000 ||
        p.timeout_seconds < 5 || p.timeout_seconds > 330 ||
        p.duration_seconds < 0 || p.duration_seconds > 300 ||
        (p.duration_seconds > 0 && p.timeout_seconds < p.duration_seconds + 5) ||
        result.linger_ms < 0 || result.linger_ms > 3000)
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
        "     [--bitrate 4000000] [--timeout 30] [--linger-ms 1000]\n"
        "     [--duration 1..300] (bounded time-based stability session)\n"
        "SHARES THE ENTIRE SELECTED MONITOR with LiveKit room participants.\n"
        "Requires LIVEKIT_URL, LIVEKIT_TOKEN and compatible hardware H.264 MFT.\n"
        "No audio or disk recording. Duration max 300 seconds, 18000 frames.\n";
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
    char duration_flag[] = "--duration", duration_seconds[] = "120";
    char* long_session[] = {app, publish, monitor, zero, consent, yes,
                            duration_flag, duration_seconds};
    const auto bounded = parse(8, long_session);
    if (bounded.profile.duration_seconds != 120 ||
        bounded.profile.frames != 3600 ||
        bounded.profile.timeout_seconds != 135) return 1;
    char invalid_duration[] = "301";
    char* over_limit[] = {app, publish, monitor, zero, consent, yes,
                          duration_flag, invalid_duration};
    try { (void)parse(8, over_limit); return 1; }
    catch (const std::invalid_argument&) {}
    char short_timeout[] = "90";
    char timeout_flag[] = "--timeout";
    char* inadequate_timeout[] = {app, publish, monitor, zero, consent, yes,
                                   duration_flag, duration_seconds,
                                   timeout_flag, short_timeout};
    try { (void)parse(10, inadequate_timeout); return 1; }
    catch (const std::invalid_argument&) {}
    char huge[] = "2147483647", fps_flag[] = "--fps";
    char* overflow_profile[] = {app, publish, monitor, zero, consent, yes,
                               fps_flag, huge, duration_flag, duration_seconds};
    try { (void)parse(10, overflow_profile); return 1; }
    catch (const std::invalid_argument&) {}
    char denied[] = "no";
    char* bad[] = {app, publish, monitor, zero, consent, denied};
    try { (void)parse(6, bad); return 1; }
    catch (const std::invalid_argument&) {}
    if (!gcp::transport::frame_arrival_metrics_self_test()) return 1;
    livekit::initialize(livekit::LogLevel::Warn);
    livekit::shutdown();
    std::cout << "H.264 access-unit, timing and consent self-test passed\n";
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

        const auto measurement_start = std::chrono::steady_clock::now();
        const auto process_start = gcp::probe::take_snapshot();
        gcp::transport::FrameArrivalMetrics packet_arrivals(measurement_start);
        std::uint64_t accepted = 0, rejected = 0, skipped = 0;
        std::uint64_t accepted_bytes = 0, accepted_keyframes = 0;
        std::uint64_t ignored_bitrate_feedback = 0;
        gcp::media::HardwareH264FeedbackStats feedback_stats;
        std::int64_t last_ts = -1;
        bool first_idr_sent = false;
        const gcp::media::HardwareH264Callback emit =
            [&](const gcp::media::HardwareH264AccessUnit& encoded) -> bool {
                if (interrupted.load()) return false;
                auto verified = gcp::transport::verify_h264_access_unit(
                    encoded.bytes, encoded.parameter_sets);
                if (encoded.keyframe && !verified.keyframe)
                    throw std::runtime_error("Encoder's keyframe metadata is inconsistent");
                if (encoded.timestamp_us < 0 || encoded.timestamp_us < last_ts)
                    throw std::runtime_error("Invalid H.264 output timestamp");
                last_ts = encoded.timestamp_us;

                // Feedback is polled BEFORE ProcessInput in the encoder loop,
                // not while emitting packets that have already been encoded.
                if (!first_idr_sent && !verified.keyframe) {
                    ++skipped;
                    return true;
                }
                livekit::EncodedVideoSource::Frame frame;
                frame.is_keyframe = verified.keyframe;
                frame.timestamp_us = encoded.timestamp_us;
                frame.data = std::move(verified.annexb);
                if (!source->captureFrame(frame)) {
                    ++rejected;
                    return false;
                }
                ++accepted;
                accepted_bytes += frame.data.size();
                packet_arrivals.record(std::chrono::steady_clock::now());
                if (verified.keyframe) {
                    first_idr_sent = true;
                    ++accepted_keyframes;
                }
                return true;
            };

        const gcp::media::HardwareH264FeedbackCallback fetch_feedback = [&]() {
            gcp::media::HardwareH264Feedback requested;
            const auto feedback = source->takeFeedback();
            requested.force_keyframe = feedback.keyframe_requested;
            if (feedback.rate_control.has_value()) {
                const auto bitrate = feedback.rate_control->target_bitrate_bps;
                // The native control layer supports only bounded UINT32
                // bitrates. Reject unsupported targets explicitly.
                if (bitrate >= 100'000 && bitrate <= 16'000'000) {
                    requested.target_bitrate_bps =
                        static_cast<std::uint32_t>(bitrate);
                } else {
                    ++ignored_bitrate_feedback;
                }
            }
            return requested;
        };

        std::cerr << "SCREEN SHARING ACTIVE: monitor " << p.monitor_index
                  << " is transmitted to the LiveKit room. Ctrl+C to stop.\n";
        const auto result = gcp::media::encode_monitor_h264(
            p, emit, fetch_feedback, &feedback_stats);
        const auto measurement_end = std::chrono::steady_clock::now();
        const auto process_end = gcp::probe::take_snapshot();
        const auto seconds = std::chrono::duration<double>(
            measurement_end - measurement_start).count();
        const auto timing = packet_arrivals.report(measurement_end);
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"probe\":\"hardware_h264_livekit\""
                  << ",\"packets_accepted\":" << accepted
                  << ",\"packets_rejected\":" << rejected
                  << ",\"accepted_encoded_bytes\":" << accepted_bytes
                  << ",\"accepted_keyframes\":" << accepted_keyframes
                  << ",\"sender_observation_seconds\":" << seconds
                  << ",\"accepted_packet_fps\":"
                  << (seconds > 0 ? accepted / seconds : 0.0)
                  << ",\"accepted_encoded_bits_per_second\":"
                  << (seconds > 0 ? accepted_bytes * 8.0 / seconds : 0.0)
                  << ",\"first_accepted_packet_ms\":" << timing.time_to_first_ms
                  << ",\"packet_arrival_p50_ms\":" << timing.interval_p50_ms
                  << ",\"packet_arrival_p95_ms\":" << timing.interval_p95_ms
                  << ",\"sender_cpu_percent_total_machine\":"
                  << gcp::probe::total_machine_cpu_percent(
                         process_start, process_end, seconds,
                         gcp::probe::logical_processor_count())
                  << ",\"sender_private_bytes_start\":" << process_start.private_bytes
                  << ",\"sender_private_bytes_end\":" << process_end.private_bytes
                  << ",\"pre_idr_skipped\":" << skipped
                  << ",\"keyframe_requests\":" << feedback_stats.keyframe_requests
                  << ",\"keyframe_controls_accepted\":" << feedback_stats.keyframe_controls_accepted
                  << ",\"keyframe_controls_unsupported\":" << feedback_stats.keyframe_controls_unsupported
                  << ",\"bitrate_requests\":" << feedback_stats.bitrate_requests
                  << ",\"bitrate_controls_accepted\":" << feedback_stats.bitrate_controls_accepted
                  << ",\"bitrate_controls_unsupported\":" << feedback_stats.bitrate_controls_unsupported
                  << ",\"out_of_range_bitrate_feedback\":" << ignored_bitrate_feedback
                  << ",\"gpu_encoder_engine_verified\":false"
                  << ",\"receiver_playback_verified\":false}\n";
        // SDK acceptance is asynchronous; retain the room briefly after
        // the finite encoder run so packets have an opportunity to forward.
        // This still does NOT prove SFU delivery or remote playback.
        if (result == 0 && accepted > 0) {
            const auto linger_until = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(options.linger_ms);
            while (!interrupted.load() &&
                   std::chrono::steady_clock::now() < linger_until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
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
    } catch (const std::exception&) {
        std::cerr << "Hardware LiveKit diagnostic failed; check configuration and capture profile.\n";
        return 1;
    }
}
