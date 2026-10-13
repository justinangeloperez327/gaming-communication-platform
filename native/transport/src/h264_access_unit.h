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
// A keyframe must include SPS/PPS either in the AU or in validated
// MF_MT_MPEG_SEQUENCE_HEADER Annex-B metadata, prepended to that IDR.
// This is a fail-closed prototype, not a general production H.264 parser.
VerifiedAccessUnit verify_h264_access_unit(
    const std::vector<std::uint8_t>& encoded,
    const std::vector<std::uint8_t>& sequence_header = {});

bool h264_access_unit_self_test();
}
