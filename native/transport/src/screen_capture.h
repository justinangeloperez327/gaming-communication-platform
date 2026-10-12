#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace gcp::transport {

struct ScreenFrame {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
};

struct ScreenCaptureStats {
    std::uint64_t arrivals = 0;
    std::uint64_t copied = 0;
    std::uint64_t throttled = 0;
    std::uint64_t rejected = 0;
    std::uint64_t failures = 0;
    std::uint64_t readback_microseconds = 0;
    std::uint64_t readback_p50_microseconds = 0;
    std::uint64_t readback_p95_microseconds = 0;
};

// Explicitly authorized, bounded test pipeline. Uses a staging D3D11
// texture and GPU -> CPU readback. Not a zero-copy/hardware-encoder solution.
class ScreenCapture {
public:
    ScreenCapture(int monitor_index, int max_fps);
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    void start();
    bool take_latest(std::uint64_t& previous_sequence, ScreenFrame& target);
    ScreenCaptureStats stats() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gcp::transport
