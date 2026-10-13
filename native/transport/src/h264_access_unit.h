#pragma once

#include <cstdint>
#include <vector>

namespace gcp::transport {
struct VerifiedAccessUnit {
    std::vector<std::uint8_t> annexb;
    bool keyframe = false;
    bool contains_parameter_sets = false;
};

// Accept Annex-B 3/4-byte start codes or AVCC 4-byte NAL length prefixes.
// Always return validated Annex-B NAL units; reject malformed/oversized input.
// A keyframe must include SPS and PPS in the *same* access unit.
// This is a fail-closed prototype, not a general production H.264 parser.
VerifiedAccessUnit verify_h264_access_unit(
    const std::vector<std::uint8_t>& encoded);

bool h264_access_unit_self_test();
}
