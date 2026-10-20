#pragma once

#include <charconv>
#include <string_view>

namespace gcp::transport {
namespace url_detail {
inline bool decimal_ipv4(std::string_view host) {
    int count = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto part = host.substr(0, dot);
        unsigned value = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || (part.size() > 1 && part.front() == '0') ||
            result.ec != std::errc{} || result.ptr != part.data() + part.size() || value > 255)
            return false;
        ++count;
        if (dot == std::string_view::npos) return count == 4;
        host.remove_prefix(dot + 1);
    }
    return false;
}

inline int ipv6_groups(std::string_view side, bool allow_ipv4) {
    if (side.empty()) return 0;
    int count = 0;
    while (!side.empty()) {
        const auto colon = side.find(':');
        const auto group = side.substr(0, colon);
        if (group.find('.') != std::string_view::npos) {
            if (!allow_ipv4 || colon != std::string_view::npos || !decimal_ipv4(group)) return -1;
            return count + 2;
        }
        if (group.empty() || group.size() > 4) return -1;
        for (const char c : group)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return -1;
        ++count;
        if (colon == std::string_view::npos) return count;
        side.remove_prefix(colon + 1);
        if (side.empty()) return -1;
    }
    return count;
}

inline bool ipv6(std::string_view host) {
    const auto compression = host.find("::");
    if (compression == std::string_view::npos) return ipv6_groups(host, true) == 8;
    const int left = ipv6_groups(host.substr(0, compression), false);
    const int right = ipv6_groups(host.substr(compression + 2), true);
    return left >= 0 && right >= 0 && left + right < 8;
}

inline bool dns_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.back() == '.') host.remove_suffix(1);
    if (host.empty()) return false;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (const char c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
        if (dot == std::string_view::npos) return true;
        host.remove_prefix(dot + 1);
    }
    return false;
}

inline bool localhost(std::string_view host) {
    constexpr std::string_view expected = "localhost";
    if (host.size() != expected.size()) return false;
    for (std::size_t i = 0; i < host.size(); ++i) {
        const char c = host[i];
        if (c != expected[i] && c != expected[i] - ('a' - 'A')) return false;
    }
    return true;
}
} // namespace url_detail

// Deliberately accepts server origins, not arbitrary URLs. No decoding or
// DNS lookup: ambiguous authorities never reach the SDK with a bearer token.
inline bool permitted_livekit_url(std::string_view url) {
    if (url.empty() || url.size() > 2048) return false;
    for (const unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || c == '@' || c == '\\' || c == '%' || c == '?' || c == '#')
            return false;
    const bool secure = url.starts_with("wss://");
    if (!secure && !url.starts_with("ws://")) return false;
    url.remove_prefix(secure ? 6 : 5);
    if (url.ends_with('/')) url.remove_suffix(1);
    if (url.empty() || url.find('/') != std::string_view::npos) return false;
    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (url.front() == '[') {
        const auto close = url.find(']');
        if (close == std::string_view::npos) return false;
        host = url.substr(1, close - 1);
        if (!secure || !url_detail::ipv6(host)) return false;
        const auto rest = url.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            has_port = true;
            port = rest.substr(1);
        }
    } else {
        const auto colon = url.find(':');
        host = url.substr(0, colon);
        if (!url_detail::dns_host(host)) return false;
        if (!secure && host != "127.0.0.1" && !url_detail::localhost(host)) return false;
        if (colon != std::string_view::npos) {
            has_port = true;
            port = url.substr(colon + 1);
        }
    }
    if (has_port) {
        unsigned value = 0;
        const auto result = std::from_chars(port.data(), port.data() + port.size(), value);
        if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
            value == 0 || value > 65535) return false;
    }
    return true;
}
} // namespace gcp::transport
