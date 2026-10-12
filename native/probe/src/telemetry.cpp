#include "telemetry.h"

#include <Psapi.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace gcp::probe {
namespace {
std::uint64_t ticks(FILETIME time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}
}

Snapshot take_snapshot() {
    const auto process = GetCurrentProcess();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (!GetProcessMemoryInfo(
            process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            static_cast<DWORD>(sizeof(memory)))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return {ticks(kernel) + ticks(user),
            static_cast<std::uint64_t>(memory.PrivateUsage)};
}

unsigned logical_processor_count() {
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    return std::max(1u, information.dwNumberOfProcessors);
}

double total_machine_cpu_percent(
    Snapshot start, Snapshot end, double seconds, unsigned logical_processors) {
    if (seconds <= 0 || logical_processors == 0
        || end.process_time_100ns < start.process_time_100ns) {
        return 0.0;
    }
    constexpr double ticks_per_second = 10'000'000.0;
    const auto used = static_cast<double>(
        end.process_time_100ns - start.process_time_100ns);
    return 100.0 * used /
        (seconds * ticks_per_second * logical_processors);
}

void print_metrics(const char* kind, double seconds, std::uint64_t frames,
                   std::uint64_t packets, Snapshot start, Snapshot end,
                   int width, int height) {
    const auto cpu = total_machine_cpu_percent(
        start, end, seconds, logical_processor_count());
    std::cout << std::fixed << std::setprecision(2)
              << "{\"probe\":\"" << kind << "\""
              << ",\"elapsed_seconds\":" << seconds
              << ",\"frames\":" << frames
              << ",\"packets\":" << packets
              << ",\"observed_fps\":" << (seconds > 0 ? frames / seconds : 0)
              << ",\"cpu_percent_total_machine\":" << cpu
              << ",\"private_bytes_start\":" << start.private_bytes
              << ",\"private_bytes_end\":" << end.private_bytes
              << ",\"width\":" << width
              << ",\"height\":" << height << "}\n";
}

} // namespace gcp::probe
