#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace gcp::media {

// Hardware encoder input, intentionally finite and explicitly authorized.
// This is a diagnostic, not an always-on capture service.
struct HardwareH264Profile {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int frames = 30;
    int bitrate_bps = 4'000'000;
    int timeout_seconds = 30;
    int monitor_index = -1;
    bool monitor_capture_confirmed = false;
};

struct HardwareH264AccessUnit {
    std::vector<std::uint8_t> bytes;
    std::int64_t timestamp_us = 0;
    bool keyframe = false;
    // H.264 SPS/PPS Annex-B bytes from MF_MT_MPEG_SEQUENCE_HEADER when
    // provided by an encoder as output media-type metadata.
    std::vector<std::uint8_t> parameter_sets;
};

// Called synchronously for each completed encoded output sample.
// Returning false rejects the output and aborts the hardware diagnostic.
// Codecs, SPS/PPS and bitstream format must be validated by the consumer.
// With a callback, no encoded sample is stored by this producer.
using HardwareH264Callback =
    std::function<bool(const HardwareH264AccessUnit&)>;

struct HardwareH264Feedback {
    bool force_keyframe = false;
    // Zero means no bitrate update requested.
    std::uint32_t target_bitrate_bps = 0;
};
using HardwareH264FeedbackCallback = std::function<HardwareH264Feedback()>;

struct HardwareH264FeedbackStats {
    std::uint64_t keyframe_requests = 0;
    std::uint64_t keyframe_controls_accepted = 0;
    std::uint64_t keyframe_controls_unsupported = 0;
    std::uint64_t bitrate_requests = 0;
    std::uint64_t bitrate_controls_accepted = 0;
    std::uint64_t bitrate_controls_unsupported = 0;
};

// Return 0 only after the hardware-class MFT encodes frames, all requested
// samples are drained, and every callback succeeds.
// The callback is required. No software MFT fallback is attempted.
int encode_monitor_h264(const HardwareH264Profile& profile,
                        const HardwareH264Callback& callback,
                        const HardwareH264FeedbackCallback& feedback = {},
                        HardwareH264FeedbackStats* feedback_stats = nullptr);

} // namespace gcp::media
