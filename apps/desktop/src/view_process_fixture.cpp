// Test-only video protocol peer. No SDK, capture, devices or network.
#include "../../../native/transport/src/view_control.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <thread>
namespace {
void event(const std::string& text) { std::cout << "GCP_VIEW " << text << '\n' << std::flush; }
}
int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--view") return 1;
    for (const auto* key : {"DATABASE_URL", "LIVEKIT_API_KEY", "LiVeKiT_ApI_SeCrEt", "LIVEKIT_TOKEN", "GCP_ACCESS_TOKEN", "GCP_MEDIA_SECRET"}) if (std::getenv(key)) return 2;
    const std::string mode = std::getenv("GCP_VIEW_FIXTURE_MODE") ? std::getenv("GCP_VIEW_FIXTURE_MODE") : "";
    gcp::transport::ViewControl control(gcp::transport::screen_now_ms());
    std::string line; std::uint64_t sequence = 0;
    while (std::getline(std::cin, line)) {
        if (line == "stop" && mode == "hang") for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!control.accept(line, gcp::transport::screen_now_ms())) return 3;
        if (line == "stop") return 0;
        if (control.take_grant()) event("connected");
        if (line.starts_with("track ")) {
            if (mode == "failed") { event("failed"); return 0; }
            event("subscribed " + control.track_sid());
            if (mode == "unsolicited") event("frame 1 160 88 0 56320");
        }
        if (line == "next") {
            if (mode == "static" && sequence) continue;
            if (!control.take_credit()) return 4;
            ++sequence;
            if (mode == "oversized") { event("frame 1 2000 1080 0 8640000"); continue; }
            if (mode == "bad_rotation") { event("frame 1 160 88 17 56320"); continue; }
            const auto width = mode == "resize" && sequence > 1 ? 320 : 160;
            const auto height = mode == "resize" && sequence > 1 ? 180 : 88;
            const auto rotate = mode == "rotate" ? 90 : 0;
            const auto size = width * height * 4;
            event("frame " + std::to_string(sequence) + " " + std::to_string(width) + " " + std::to_string(height) + " " + std::to_string(rotate) + " " + std::to_string(size));
            if (mode == "partial") { std::cout << "12345678" << std::flush; continue; }
            std::string bytes(static_cast<std::size_t>(size), '\0');
            for (std::size_t i = 0; i < bytes.size(); i += 4) { bytes[i] = 30; bytes[i + 1] = static_cast<char>(160); bytes[i + 2] = 90; bytes[i + 3] = static_cast<char>(255); }
            if (mode == "fragmented") {
                for (std::size_t i = 0; i < bytes.size(); i += 4096) { std::cout.write(bytes.data() + i, static_cast<std::streamsize>((std::min)(std::size_t{4096}, bytes.size() - i))); std::cout.flush(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            } else std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) << std::flush;
            if (mode == "oversend") event("frame " + std::to_string(sequence + 1) + " 160 88 0 56320");
        }
    }
    return 0;
}
