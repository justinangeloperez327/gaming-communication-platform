#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcp::transport {

struct H264AccessUnit {
    std::vector<std::uint8_t> bytes;
    bool keyframe = false;
};

// Strict, bounded parser for test files where H.264 access units are
// delimited by AUD NAL units (type 9). This is intentionally not a general
// H.264 parser. An IDR access unit must include its own SPS/PPS.
std::vector<H264AccessUnit> split_annexb_access_units(
    const std::vector<std::uint8_t>& bytes);

bool test_annexb_h264_parser();

} // namespace gcp::transport
