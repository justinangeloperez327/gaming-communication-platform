#pragma once

#include <Windows.h>
#include <cstdint>

namespace gcp::probe {

struct Snapshot {
    std::uint64_t process_time_100ns{};
    std::uint64_t private_bytes{};
};

Snapshot take_snapshot();
double total_machine_cpu_percent(
    Snapshot start, Snapshot end, double seconds, unsigned logical_processors);
unsigned logical_processor_count();
void print_metrics(const char* kind, double seconds, std::uint64_t frames,
                   std::uint64_t packets, Snapshot start, Snapshot end,
                   int width = 0, int height = 0);

} // namespace gcp::probe
