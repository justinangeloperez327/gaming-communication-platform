#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Clock-only, bounded inter-arrival metrics. No pixels, identifiers, tokens,
// timestamps from the network, or unbounded sample allocations are retained.
// NOT an end-to-end latency estimate; receiver clock != sender clock.
namespace gcp::transport {

class FrameArrivalMetrics {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Report {
        std::uint64_t frames = 0;
        std::uint64_t interval_samples = 0;
        double time_to_first_ms = 0.0;
        double observed_frame_fps = 0.0;
        double interval_p50_ms = 0.0;
        double interval_p95_ms = 0.0;
        double interval_max_ms = 0.0;
    };

    explicit FrameArrivalMetrics(TimePoint start) : start_(start) {}

    void record(TimePoint time) {
        if (time < start_) return;
        if (frames_ != 0 && time >= last_) {
            const auto delta = std::chrono::duration<double, std::milli>(
                time - last_).count();
            intervals_[static_cast<std::size_t>(
                observed_intervals_ % intervals_.size())] = delta;
            ++observed_intervals_;
        }
        if (frames_ == 0) first_ = time;
        last_ = time;
        ++frames_;
    }

    Report report(TimePoint end) const {
        Report out;
        out.frames = frames_;
        if (frames_ == 0) return out;
        out.time_to_first_ms = std::chrono::duration<double, std::milli>(
            first_ - start_).count();
        if (end > first_) {
            const auto elapsed = std::chrono::duration<double>(
                end - first_).count();
            out.observed_frame_fps = elapsed > 0 ? frames_ / elapsed : 0.0;
        }
        const auto used = static_cast<std::size_t>(
            (std::min<std::uint64_t>)(observed_intervals_, intervals_.size()));
        out.interval_samples = used;
        if (used == 0) return out;
        auto sorted = intervals_;
        std::sort(sorted.begin(), sorted.begin() +
                  static_cast<std::ptrdiff_t>(used));
        out.interval_p50_ms = sorted[(used - 1) / 2];
        out.interval_p95_ms = sorted[((used - 1) * 95) / 100];
        out.interval_max_ms = sorted[used - 1];
        return out;
    }

private:
    TimePoint start_;
    TimePoint first_{};
    TimePoint last_{};
    std::uint64_t frames_ = 0;
    std::uint64_t observed_intervals_ = 0;
    std::array<double, 256> intervals_{};
};

inline bool frame_arrival_metrics_self_test() {
    using Clock = FrameArrivalMetrics::Clock;
    const Clock::TimePoint zero{};
    FrameArrivalMetrics stats(zero);
    auto empty = stats.report(zero);
    if (empty.frames || empty.interval_samples) return false;

    stats.record(zero + std::chrono::milliseconds(100));
    stats.record(zero + std::chrono::milliseconds(116));
    stats.record(zero + std::chrono::milliseconds(149));
    stats.record(zero + std::chrono::milliseconds(182));
    const auto result = stats.report(zero + std::chrono::milliseconds(200));
    if (result.frames != 4 || result.interval_samples != 3 ||
        result.time_to_first_ms != 100.0 ||
        result.interval_p50_ms != 33.0 ||
        result.interval_p95_ms != 33.0 ||
        result.interval_max_ms != 33.0 ||
        result.observed_frame_fps < 39.0 || result.observed_frame_fps > 41.0)
        return false;

    FrameArrivalMetrics capped(zero);
    for (int i = 0; i < 400; ++i)
        capped.record(zero + std::chrono::milliseconds(i * 20));
    const auto bounded = capped.report(
        zero + std::chrono::milliseconds(8000));
    return bounded.frames == 400 && bounded.interval_samples == 256 &&
           bounded.interval_p50_ms == 20.0 &&
           bounded.interval_p95_ms == 20.0;
}

} // namespace gcp::transport
