// Test-only protocol peer. Never opens a picker, device, SDK or network.
#include "../../../native/transport/src/screen_control.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
namespace {
void event(const char* text) { std::cout << "GCP_SCREEN " << text << '\n' << std::flush; }
void record(const char* text) {
    if (const auto* path = std::getenv("GCP_SCREEN_FIXTURE_LOG")) std::ofstream(path, std::ios::app) << text << '\n';
}
}
int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--pick-source") return 1;
    for (const auto* key : {"DATABASE_URL", "LIVEKIT_API_KEY", "LiVeKiT_ApI_SeCrEt", "LIVEKIT_TOKEN", "LIVEKIT_URL", "GCP_MEDIA_SECRET", "GCP_POSTGRES_ADMIN_PASSWORD", "GCP_ACCESS_TOKEN", "GCP_REFRESH_TOKEN"})
        if (std::getenv(key)) return 2;
    record("start");
    const std::string mode = std::getenv("GCP_SCREEN_FIXTURE_MODE") ? std::getenv("GCP_SCREEN_FIXTURE_MODE") : "";
    if (mode == "cancel") { event("cancelled"); return 0; }
    if (mode == "flood") { std::cout << std::string(20000, 'x') << std::flush; }
    if (mode != "no_source") event("source PGI+V2luZG93PC9iPg== 801 601");
    if (mode == "closed_source") { std::this_thread::sleep_for(std::chrono::milliseconds(100)); return 0; }
    gcp::transport::ScreenControl control(gcp::transport::screen_now_ms()); control.selected();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (mode == "hang" && line == "stop") for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!control.accept(line, gcp::transport::screen_now_ms())) return 3;
        if (line == "stop") { record("stop"); event("stopped"); return 0; }
        if (line.starts_with("renew ")) record("renew");
        if (control.take_grant()) {
            record("publish"); event("connected"); event("capturing");
            if (mode == "interrupted") { event("failed"); return 0; }
            if (mode == "stopped_alive") { event("stopped"); for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            if (mode != "no_frame") event("sharing");
        }
    }
    return 0;
}
