#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace gcp::voice {

constexpr int kRate = 48000;
constexpr int kChannels = 1;
constexpr int kBlockFrames = 480; // 10 ms PCM 16 mono
constexpr std::size_t kMaxBufferedSamples = 9600; // 200 ms cap

struct Counters {
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> played{0};
    std::atomic<std::uint64_t> decoded{0};
    std::atomic<std::uint64_t> overwritten{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> silence{0};
};

class PlaybackQueue {
public:
    std::size_t push(std::span<const std::int16_t> mono);
    void pop(std::span<std::int16_t> out, Counters& stats);
    std::size_t size() const;
private:
    mutable std::mutex mutex_;
    std::deque<std::int16_t> samples_;
};

bool queue_self_test();
void list_devices();
void capture_microphone(
    int device_index,
    std::atomic<bool>& running,
    Counters& stats,
    const std::function<void(std::span<const std::int16_t>)>& emit);
void play_speakers(
    int device_index,
    std::atomic<bool>& running,
    PlaybackQueue& queue,
    Counters& stats);

} // namespace gcp::voice
