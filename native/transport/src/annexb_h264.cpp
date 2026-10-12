#include "annexb_h264.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace gcp::transport {
namespace {
constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxAccessUnitBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxAccessUnits = 3600u;

struct Nal {
    std::size_t start = 0;
    std::uint8_t type = 0;
};

std::size_t prefix_length(const std::vector<std::uint8_t>& bytes,
                          std::size_t offset) {
    if (offset + 3 > bytes.size() || bytes[offset] != 0 ||
        bytes[offset + 1] != 0) return 0;
    if (bytes[offset + 2] == 1) return 3;
    if (offset + 4 <= bytes.size() && bytes[offset + 2] == 0 &&
        bytes[offset + 3] == 1) return 4;
    return 0;
}

std::vector<Nal> scan_nals(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > kMaxBytes) {
        throw std::invalid_argument("H.264 file must contain 1..64 MiB");
    }
    std::vector<Nal> nals;
    for (std::size_t at = 0; at + 3 <= bytes.size(); ++at) {
        const auto prefix = prefix_length(bytes, at);
        if (!prefix) continue;
        if (at + prefix >= bytes.size()) {
            throw std::invalid_argument("Truncated H.264 NAL header");
        }
        const auto header = bytes[at + prefix];
        const auto type = static_cast<std::uint8_t>(header & 0x1fu);
        if ((header & 0x80u) != 0 || type == 0) {
            throw std::invalid_argument("Invalid H.264 NAL header");
        }
        nals.push_back({at, type});
        at += prefix;
    }
    if (nals.empty() || nals.front().start != 0) {
        throw std::invalid_argument("Expected Annex B H.264 start code at offset 0");
    }
    return nals;
}

H264AccessUnit make_au(const std::vector<std::uint8_t>& bytes,
                       const std::vector<Nal>& nals,
                       std::size_t first_nal, std::size_t last_nal) {
    if (first_nal >= last_nal) {
        throw std::invalid_argument("Empty H.264 access unit");
    }
    const auto start = nals[first_nal].start;
    const auto end = last_nal < nals.size() ? nals[last_nal].start : bytes.size();
    if (end <= start || end - start > kMaxAccessUnitBytes) {
        throw std::invalid_argument("H.264 access unit exceeds 4 MiB");
    }
    bool has_aud = false, has_vcl = false, has_idr = false;
    bool has_sps = false, has_pps = false;
    for (auto i = first_nal; i < last_nal; ++i) {
        switch (nals[i].type) {
            case 1: has_vcl = true; break;
            case 5: has_vcl = true; has_idr = true; break;
            case 7: has_sps = true; break;
            case 8: has_pps = true; break;
            case 9: has_aud = true; break;
            default: break;
        }
    }
    if (!has_aud || !has_vcl) {
        throw std::invalid_argument("Each H.264 AU must contain an AUD and a video slice");
    }
    if (has_idr && (!has_sps || !has_pps)) {
        throw std::invalid_argument("Every IDR access unit requires SPS and PPS");
    }
    H264AccessUnit result;
    result.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                        bytes.begin() + static_cast<std::ptrdiff_t>(end));
    result.keyframe = has_idr;
    return result;
}
} // namespace

std::vector<H264AccessUnit> split_annexb_access_units(
    const std::vector<std::uint8_t>& bytes) {
    const auto nals = scan_nals(bytes);
    std::vector<H264AccessUnit> output;
    std::size_t group_start = 0;
    bool has_aud = false;
    for (std::size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type != 9) continue;
        if (has_aud) {
            if (output.size() >= kMaxAccessUnits) {
                throw std::invalid_argument("Too many encoded frames in H.264 test file");
            }
            output.push_back(make_au(bytes, nals, group_start, i));
            group_start = i;
        }
        has_aud = true;
    }
    if (!has_aud || output.size() >= kMaxAccessUnits) {
        throw std::invalid_argument("H.264 test file requires AUD-delimited frames");
    }
    output.push_back(make_au(bytes, nals, group_start, nals.size()));
    if (output.empty() || !output.front().keyframe) {
        throw std::invalid_argument("H.264 test file must begin with an IDR keyframe");
    }
    return output;
}

bool test_annexb_h264_parser() {
    const std::vector<std::uint8_t> valid{
        0,0,0,1,9,0xf0,
        0,0,1,7,0x42,0x00,
        0,0,1,8,0xce,0x06,
        0,0,1,5,0x88,
        0,0,0,1,9,0xf0,
        0,0,1,1,0x88
    };
    const auto frames = split_annexb_access_units(valid);
    if (frames.size() != 2 || !frames[0].keyframe || frames[1].keyframe ||
        frames[0].bytes.size() != 23 || frames[1].bytes.size() != 11) {
        return false;
    }
    try {
        (void)split_annexb_access_units({0,0,1,5,0x88});
        return false;
    } catch (const std::invalid_argument&) {
    }
    try {
        (void)split_annexb_access_units({
            0,0,1,9,0xf0, 0,0,1,5,0x88
        });
        return false;
    } catch (const std::invalid_argument&) {
    }
    return true;
}
} // namespace gcp::transport
