#include "livekit_url.h"
#include <array>
#include <iostream>
#include <string>

int main() {
    constexpr auto allowed = std::to_array<std::string_view>({
        "wss://voice.example.com", "wss://voice.example.com:443/",
        "wss://localhost", "wss://127.0.0.1:7880", "ws://127.0.0.1:7880",
        "ws://localhost:7880/", "ws://LOCALHOST", "ws://127.0.0.1",
        "wss://[::1]:7880", "wss://[2001:db8::1]/",
        "wss://[1:2:3:4:5:6:7:8]", "wss://[::ffff:192.0.2.1]:443",
        "wss://my-sfu.internal:65535", "wss://example.com."
    });
    constexpr auto denied = std::to_array<std::string_view>({
        "", "wss://", "https://example.com", "ws://example.com:7880",
        "ws://127.0.0.1:7880@example.invalid", "ws://localhost:7880@example.invalid",
        "wss://user:password@example.com", "wss://@example.com",
        "ws://localhost.evil:7880", "ws://127.0.0.1.evil:7880", "ws://127.1:7880",
        "ws://2130706433:7880", "ws://127.0.0.01:7880", "ws://localhost.:7880",
        "ws://[::1]:7880", "wss://example.com/path", "wss://example.com//",
        "wss://example.com?token=x", "wss://example.com#fragment",
        "wss://example.com?", "wss://example.com#", "wss://example.com\\@evil",
        "wss://example%2ecom", "wss://%65xample.com", "wss://example.com%2f@evil",
        "wss://example.com:", "wss://example.com:0", "wss://example.com:65536",
        "wss://example.com:-1", "wss://example.com:+443", "wss://example.com:443:80",
        "wss://example.com:443junk", "wss://example.com:999999999999999999999",
        "wss://-example.com", "wss://example-.com", "wss://example..com",
        "wss://example.com..", "wss://.", "wss:///example.com", "wss:////evil",
        "wss://[::1]evil", "wss://[::1]:443@evil", "wss://[::1", "wss://[]",
        "wss://[::::]", "wss://[1:2:3:4:5:6:7]", "wss://[1:2:3:4:5:6:7:8:9]",
        "wss://[1:2:3:4:5:6:7::8]", "wss://[1::2::3]", "wss://[::gggg]",
        "wss://[::ffff:256.1.1.1]", "wss://[::ffff:01.2.3.4]", "wss://[fe80::1%25eth0]"
    });
    for (const auto url : allowed) {
        if (!gcp::transport::permitted_livekit_url(url)) {
            std::cerr << "Rejected permitted fixture: " << url << '\n';
            return 1;
        }
    }
    for (const auto url : denied) {
        if (gcp::transport::permitted_livekit_url(url)) {
            std::cerr << "Accepted unsafe fixture: " << url << '\n';
            return 1;
        }
    }
    // Include embedded NUL, control characters and every non-ASCII byte.
    for (unsigned value = 0; value <= 255; ++value) {
        if (value > 0x20 && value < 0x7f) continue;
        std::string url = "wss://example.com";
        url.insert(8, 1, static_cast<char>(value));
        if (gcp::transport::permitted_livekit_url(url)) return 1;
    }
    if (gcp::transport::permitted_livekit_url("wss://" + std::string(2048, 'a'))) return 1;
    std::cout << "Native LiveKit URL authority policy tests passed\n";
}
