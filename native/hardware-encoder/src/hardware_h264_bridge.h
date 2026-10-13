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
};

// Called synchronously for each completed encoded output sample.
// Returning false rejects the output and aborts the hardware diagnostic.
// Codecs, SPS/PPS and bitstream format must be validated by the consumer.
// With a callback, no encoded sample is stored by this producer.
using HardwareH264Callback =
    std::function<bool(const HardwareH264AccessUnit&)>;

// Return 0 only after the hardware-class MFT encodes frames, all requested
// samples are drained, and every callback succeeds.
// The callback is required. No software MFT fallback is attempted.
int encode_monitor_h264(const HardwareH264Profile& profile,
                        const HardwareH264Callback& callback);

} // namespace gcp::media
