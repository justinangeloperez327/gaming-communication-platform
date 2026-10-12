#include "copy_benchmark.h"
#include "telemetry.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

namespace gcp::probe {
int run_copy_benchmark(int width, int height, int iterations) {
    if (width <= 0 || height <= 0 || iterations <= 0) return 1;
    const auto bytes = static_cast<std::size_t>(width) *
                       static_cast<std::size_t>(height) * 4u;
    std::vector<std::uint8_t> from(bytes, 0x9A);
    std::vector<std::uint8_t> into(bytes);
    std::uint64_t checksum = 0;
    const auto before = take_snapshot();
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        // Force data-dependent use so the compiler cannot omit the copies.
        from[static_cast<std::size_t>(i) % bytes] = static_cast<std::uint8_t>(i);
        std::memcpy(into.data(), from.data(), bytes);
        checksum += into[static_cast<std::size_t>(i) % bytes];
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto after = take_snapshot();
    const auto rate = elapsed > 0 ? (static_cast<double>(bytes) * iterations /
        (1024.0 * 1024.0 * 1024.0 * elapsed)) : 0.0;
    std::cout << std::fixed << std::setprecision(3)
              << "{\"probe\":\"cpu_bgra_copy\""
              << ",\"width\":" << width
              << ",\"height\":" << height
              << ",\"iterations\":" << iterations
              << ",\"frame_bytes\":" << bytes
              << ",\"elapsed_seconds\":" << elapsed
              << ",\"throughput_gib_per_second\":" << rate
              << ",\"cpu_percent_total_machine\":"
              << total_machine_cpu_percent(
                  before, after, elapsed, logical_processor_count())
              << ",\"private_bytes_end\":" << after.private_bytes
              << ",\"checksum\":" << checksum << "}\n";
    return checksum == 0 && iterations > 1 ? 2 : 0;
}
} // namespace gcp::probe
