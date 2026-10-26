#pragma once
#include "screen_control.h"
#include <array>
#include <algorithm>
#include <utility>

namespace gcp::transport {
inline bool screen_track_sid(std::string_view sid) {
    return sid.starts_with("TR_") && sid.size() >= 4 && sid.size() <= 128 &&
        std::all_of(sid.begin(), sid.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}
struct ViewGrant {
    std::string url, token, room, identity, publisher;
};
class ViewControl final {
public:
    explicit ViewControl(std::int64_t now) : heartbeat_(now) {}
    void stop() { std::lock_guard lock(mutex_); stopped_ = true; }
    bool alive(std::int64_t now) const { std::lock_guard lock(mutex_); return alive_locked(now); }
    bool accept(std::string_view line, std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (!alive_locked(now)) { stopped_ = true; return false; }
        if (line == "stop") { stopped_ = true; return true; }
        if (line == "ping") { heartbeat_ = now; return true; }
        if (line == "next" && started_ && !sid_.empty() && !credit_) { credit_ = true; return true; }
        if (line.starts_with("track ") && started_ && sid_.empty() && screen_track_sid(line.substr(6))) {
            sid_ = line.substr(6); return true;
        }
        std::vector<std::string_view> fields;
        if (line.empty() || line.size() > 12288 || line.front() == ' ' || line.back() == ' ') { stopped_ = true; return false; }
        while (!line.empty() && fields.size() <= 7) {
            const auto space = line.find(' '); fields.push_back(line.substr(0, space));
            if (space == std::string_view::npos) break;
            line.remove_prefix(space + 1);
        }
        std::int64_t deadline = 0;
        const auto valid_deadline = [&] {
            if (fields.size() < 2) return false;
            const auto part = fields[1];
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), deadline);
            return error == std::errc{} && end == part.data() + part.size() && deadline > now && deadline - now <= 60000;
        };
        if (fields.size() == 2 && fields[0] == "renew" && started_ && valid_deadline()) { deadline_ = deadline; return true; }
        if (fields.size() == 7 && fields[0] == "watch" && !started_ && valid_deadline() &&
            permitted_livekit_url(fields[2]) && screen_jwt(fields[3]) &&
            fields[4].starts_with("gcp-screen-") && screen_uuid(fields[4].substr(11)) &&
            fields[5].starts_with("gcp-screen-view-") && screen_uuid(fields[5].substr(16)) &&
            fields[6].starts_with("gcp-screen-publish-") && screen_uuid(fields[6].substr(19))) {
            grant_ = ViewGrant{std::string(fields[2]), std::string(fields[3]), std::string(fields[4]), std::string(fields[5]), std::string(fields[6])};
            started_ = true; deadline_ = deadline; return true;
        }
        stopped_ = true; return false;
    }
    std::optional<ViewGrant> take_grant() { std::lock_guard lock(mutex_); auto value = std::move(grant_); grant_.reset(); return value; }
    std::string track_sid() const { std::lock_guard lock(mutex_); return sid_; }
    bool take_credit() { std::lock_guard lock(mutex_); return std::exchange(credit_, false); }
private:
    bool alive_locked(std::int64_t now) const {
        return !stopped_ && now >= heartbeat_ && now - heartbeat_ <= 5000 && (!started_ || now < deadline_);
    }
    mutable std::mutex mutex_;
    std::int64_t heartbeat_, deadline_ = 0;
    bool stopped_ = false, started_ = false, credit_ = false;
    std::string sid_;
    std::optional<ViewGrant> grant_;
};
struct ViewFrameHeader {
    std::uint64_t sequence = 0;
    int width = 0, height = 0, rotation = 0;
    std::size_t bytes = 0;
};
inline std::optional<ViewFrameHeader> view_frame_header(std::string_view line) {
    std::array<std::uint64_t, 5> fields{};
    if (!line.starts_with("frame ")) return std::nullopt;
    line.remove_prefix(6);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto space = line.find(' '); const auto part = line.substr(0, space);
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), fields[i]);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() ||
            (i + 1 < fields.size() ? space == std::string_view::npos : space != std::string_view::npos)) return std::nullopt;
        if (space != std::string_view::npos) line.remove_prefix(space + 1);
    }
    if (!fields[0] || !fields[1] || fields[1] > 1920 || !fields[2] || fields[2] > 1080 ||
        (fields[3] != 0 && fields[3] != 90 && fields[3] != 180 && fields[3] != 270) || fields[4] != fields[1] * fields[2] * 4) return std::nullopt;
    return ViewFrameHeader{fields[0], static_cast<int>(fields[1]), static_cast<int>(fields[2]), static_cast<int>(fields[3]), static_cast<std::size_t>(fields[4])};
}
} // namespace gcp::transport
