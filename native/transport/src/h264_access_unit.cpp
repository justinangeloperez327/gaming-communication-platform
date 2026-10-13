#include "h264_access_unit.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace gcp::transport {
namespace {
constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxNals = 512;

std::size_t prefix_at(const std::vector<std::uint8_t>& b, std::size_t i) {
    if (i + 3 > b.size() || b[i] != 0 || b[i + 1] != 0) return 0;
    if (b[i + 2] == 1) return 3;
    if (i + 4 <= b.size() && b[i + 2] == 0 && b[i + 3] == 1) return 4;
    return 0;
}

struct NalSummary {
    bool slice = false;
    bool idr = false;
    bool sps = false;
    bool pps = false;
    std::size_t count = 0;
    void append_type(const std::uint8_t* data, std::size_t size) {
        if (!size || (data[0] & 0x80u) || (data[0] & 0x1fu) == 0)
            throw std::invalid_argument("Invalid H.264 NAL unit");
        const auto type = data[0] & 0x1fu;
        if (type == 1 || type == 5) slice = true;
        if (type == 5) idr = true;
        if (type == 7) sps = true;
        if (type == 8) pps = true;
        if (++count > kMaxNals)
            throw std::invalid_argument("H.264 AU has too many NAL units");
    }
};

void append_nal(VerifiedAccessUnit& result, NalSummary& summary,
                const std::vector<std::uint8_t>& bytes,
                std::size_t from, std::size_t to) {
    if (from >= to || to > bytes.size())
        throw std::invalid_argument("Invalid H.264 NAL bounds");
    summary.append_type(bytes.data() + from, to - from);
    result.annexb.insert(result.annexb.end(), {0, 0, 0, 1});
    result.annexb.insert(result.annexb.end(),
                         bytes.begin() + static_cast<std::ptrdiff_t>(from),
                         bytes.begin() + static_cast<std::ptrdiff_t>(to));
}

VerifiedAccessUnit validate(const std::vector<std::uint8_t>& bytes,
                            bool has_start_codes) {
    VerifiedAccessUnit result;
    NalSummary summary;
    if (has_start_codes) {
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto marker = prefix_at(bytes, at);
            if (!marker || at + marker >= bytes.size())
                throw std::invalid_argument("Malformed Annex B H.264 start code");
            const auto start = at + marker;
            std::size_t next = start;
            while (next < bytes.size() && !prefix_at(bytes, next)) ++next;
            if (next == bytes.size()) {
                // Permitted Annex-B trailing_zero_8bits are not part of the NAL.
                while (next > start + 1 && bytes[next - 1] == 0) --next;
            }
            append_nal(result, summary, bytes, start, next);
            at = next;
            // Skip terminal trailing zeros, if present.
            if (bytes.size() - at <= 3) {
                bool trailing_zero = true;
                for (auto i = at; i < bytes.size(); ++i)
                    if (bytes[i] != 0) trailing_zero = false;
                if (trailing_zero) break;
            }
        }
    } else {
        std::size_t at = 0;
        while (at < bytes.size()) {
            if (at + 4 > bytes.size())
                throw std::invalid_argument("Truncated AVCC NAL length");
            const auto size = (std::size_t(bytes[at]) << 24) |
                              (std::size_t(bytes[at + 1]) << 16) |
                              (std::size_t(bytes[at + 2]) << 8) |
                               std::size_t(bytes[at + 3]);
            at += 4;
            if (!size || size > bytes.size() - at)
                throw std::invalid_argument("Invalid AVCC NAL size");
            append_nal(result, summary, bytes, at, at + size);
            at += size;
        }
    }
    if (!summary.slice) throw std::invalid_argument("H.264 AU has no video slice");
    if (summary.idr && (!summary.sps || !summary.pps))
        throw std::invalid_argument("H.264 IDR is missing SPS/PPS");
    if (result.annexb.size() > kMaxBytes)
        throw std::invalid_argument("Normalized H.264 AU too large");
    result.keyframe = summary.idr;
    result.contains_parameter_sets = summary.sps && summary.pps;
    return result;
}
} // namespace

VerifiedAccessUnit verify_h264_access_unit(
    const std::vector<std::uint8_t>& encoded) {
    if (encoded.empty() || encoded.size() > kMaxBytes)
        throw std::invalid_argument("H.264 access unit size is invalid");
    const bool annexb = prefix_at(encoded, 0) != 0;
    return validate(encoded, annexb);
}

bool h264_access_unit_self_test() {
    const std::vector<std::uint8_t> annexb{
        0,0,0,1,0x67,0x42,0x00,
        0,0,1,0x68,0xce,
        0,0,1,0x65,0x88
    };
    const auto a = verify_h264_access_unit(annexb);
    if (!a.keyframe || !a.contains_parameter_sets || a.annexb.empty()) return false;
    const std::vector<std::uint8_t> avcc{
        0,0,0,3,0x67,0x42,0x00,
        0,0,0,2,0x68,0xce,
        0,0,0,2,0x65,0x88
    };
    const auto b = verify_h264_access_unit(avcc);
    if (!b.keyframe || b.annexb != a.annexb) return false;
    const auto delta = verify_h264_access_unit({0,0,1,0x41,0x88});
    if (delta.keyframe || delta.contains_parameter_sets) return false;
    try {
        (void)verify_h264_access_unit({0,0,0,1,0x65,0x88});
        return false;
    } catch (const std::invalid_argument&) {}
    try {
        (void)verify_h264_access_unit({0,0,0,8,0x65,0x88});
        return false;
    } catch (const std::invalid_argument&) {}
    return true;
}
} // namespace gcp::transport
