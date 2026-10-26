#pragma once
#include "livekit_url.h"
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gcp::transport {
inline std::int64_t screen_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool screen_uuid(std::string_view id) {
    if (id.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else {
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
            nonzero = nonzero || id[i] != '0';
        }
    }
    return nonzero;
}
inline bool screen_jwt(std::string_view token) {
    if (token.size() < 32 || token.size() > 8192) return false;
    unsigned dots = 0;
    bool segment = false;
    for (const auto c : token) {
        if (c == '.') { if (!segment) return false; ++dots; segment = false; }
        else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
            segment = true;
        }
    }
    return dots == 2 && segment;
}
struct ScreenGrant {
    std::int64_t deadline = 0;
    std::string url, token, room, identity;
};

// Single-use consent capability, bound to the source retained by this process.
// The parent and child use the same steady-clock protocol, so IPC/HTTP delays
// cannot extend a lease. No reconnect or fresh publication after Stop.
class ScreenControl final {
public:
    explicit ScreenControl(std::int64_t now) : heartbeat_(now) {}
    void selected() { std::lock_guard lock(mutex_); selected_ = true; }
    void stop() { std::lock_guard lock(mutex_); stopped_ = true; }
    bool alive(std::int64_t now) const {
        std::lock_guard lock(mutex_);
        return alive_locked(now);
    }
    bool accept(std::string_view line, std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (!alive_locked(now)) { stopped_ = true; return false; }
        if (line == "stop") { stopped_ = true; return true; }
        if (line == "ping") { heartbeat_ = now; return true; }
        if (line.empty() || line.front() == ' ' || line.back() == ' ' || line.size() > 12288) { stopped_ = true; return false; }
        std::vector<std::string_view> fields;
        while (!line.empty() && fields.size() <= 6) {
            const auto space = line.find(' ');
            fields.push_back(line.substr(0, space));
            if (space == std::string_view::npos) break;
            line.remove_prefix(space + 1);
        }
        std::int64_t deadline = 0;
        const auto valid_deadline = [&] {
            if (fields.size() < 2) return false;
            const auto part = fields[1];
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), deadline);
            return error == std::errc{} && end == part.data() + part.size() &&
                deadline > now && deadline - now <= 60000;
        };
        if (fields.size() == 2 && fields[0] == "renew" && published_ && valid_deadline()) {
            deadline_ = deadline; return true;
        }
        if (fields.size() == 6 && fields[0] == "publish" && selected_ && !published_ && valid_deadline() &&
            permitted_livekit_url(fields[2]) && screen_jwt(fields[3]) &&
            fields[4].starts_with("gcp-screen-") && screen_uuid(fields[4].substr(11)) &&
            fields[5].starts_with("gcp-screen-publish-") && screen_uuid(fields[5].substr(19))) {
            grant_ = ScreenGrant{deadline, std::string(fields[2]), std::string(fields[3]),
                std::string(fields[4]), std::string(fields[5])};
            published_ = true; deadline_ = deadline; return true;
        }
        stopped_ = true; return false;
    }
    std::optional<ScreenGrant> take_grant() {
        std::lock_guard lock(mutex_);
        auto result = std::move(grant_); grant_.reset(); return result;
    }
private:
    bool alive_locked(std::int64_t now) const {
        return !stopped_ && now >= heartbeat_ && now - heartbeat_ <= 5000 &&
            (!published_ || now < deadline_);
    }
    mutable std::mutex mutex_;
    std::int64_t heartbeat_, deadline_ = 0;
    bool selected_ = false, published_ = false, stopped_ = false;
    std::optional<ScreenGrant> grant_;
};
} // namespace gcp::transport
