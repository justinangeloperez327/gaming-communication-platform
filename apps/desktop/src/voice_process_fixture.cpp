// Offline control-protocol fixture. Never initializes LiveKit or audio devices.
#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <chrono>
#include <thread>

int main(int argc, char **argv) {
    if (std::getenv("GCP_FIXTURE_VALIDATE_ENV")) {
        for (const auto *name : {"DATABASE_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET",
                                 "GCP_ACCESS_TOKEN", "GCP_REFRESH_TOKEN"}) {
            if (std::getenv(name)) return 10;
        }
        const auto *runtime = std::getenv("GCP_FIXTURE_RUNTIME_VALUE");
        if (!runtime || std::string_view{runtime} != "preserved") return 11;
#ifdef _WIN32
        if (!std::getenv("SystemRoot") || !std::getenv("PATH")) return 12;
#endif
        const auto *url = std::getenv("LIVEKIT_URL");
        const auto *token = std::getenv("LIVEKIT_TOKEN");
        if (argc == 2 && std::string_view{argv[1]} == "devices") {
            if (url || token) return 13;
        } else if (!url || !token || std::string_view{url} != "wss://fixture.invalid" ||
                   std::string_view{token} != std::string(64, 'x')) return 14;
    }
    if (argc == 2 && std::string_view{argv[1]} == "devices") {
        const auto *mode = std::getenv("GCP_FIXTURE_DEVICE_SCAN");
        if (mode && std::string_view{mode} == "empty") return 0;
        if (mode && std::string_view{mode} == "oversized") {
            const std::string chunk(4096, 'x');
            for (int index = 0; index < 64; ++index) std::cout << chunk << std::flush;
            // A bounded reader must terminate this child before its scan watchdog.
            std::this_thread::sleep_for(std::chrono::seconds(10));
        } else if (mode && std::string_view{mode} == "stderr") {
            const std::string chunk(4096, 'x');
            for (int index = 0; index < 1024; ++index) std::cerr << chunk;
        }
        const bool recovered = mode && std::string_view{mode} == "stderr";
        std::cout << "input [0] " << (recovered ? "Recovered microphone" : "Fixture microphone")
                  << " | id=fixture-input\noutput [0] Fixture speaker | id=fixture-output\n";
        return 0;
    }
    bool managed = false, unlimited = false, microphone = false;
    std::string_view input, output;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--desktop-control") managed = true;
        if (argument == "--publish-mic") microphone = true;
        if (argument == "--input" && index + 1 < argc) input = argv[index + 1];
        if (argument == "--output" && index + 1 < argc) output = argv[index + 1];
        if (argument == "--seconds" && index + 1 < argc &&
            std::string_view{argv[index + 1]} == "0") unlimited = true;
    }
    if (!managed || !unlimited) return 2;
    if (std::getenv("GCP_FIXTURE_EXPECT_DEVICE_ARGS") &&
        (output != "fixture-output" || (microphone ? input != "fixture-input" : !input.empty()))) return 15;
    const auto *outputMode = std::getenv("GCP_FIXTURE_VOICE_OUTPUT");
    if (outputMode && std::string_view{outputMode} == "burst") {
        const std::string line(128, 'd');
        for (int index = 0; index < 256; ++index) std::cout << line << '\n';
    }
    if (outputMode && std::string_view{outputMode} == "microphone-failed" && microphone) {
        std::cout << "GCP_VOICE_EVENT:microphone_failed" << std::endl;
        return 1;
    }
    std::cout << "GCP_VOICE_EVENT:connected\nGCP_VOICE_EVENT:muted" << std::endl;
    bool interrupted = false;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "stop") break;
        if (command == "mute" && outputMode && std::string_view{outputMode} == "mute-timeout") continue;
        if (command == "mute" || command == "mode_ptt" || command == "mode_open")
            std::cout << "GCP_VOICE_EVENT:muted" << std::endl;
        else if (command == "unmute" && microphone) {
            std::cout << "GCP_VOICE_EVENT:unmuted" << std::endl;
            if (outputMode && std::string_view{outputMode} == "reconnect" && !interrupted) {
                interrupted = true;
                std::cout << "GCP_VOICE_EVENT:reconnecting\nGCP_VOICE_EVENT:rearm_required\n"
                             "GCP_VOICE_EVENT:muted\nGCP_VOICE_EVENT:reconnected" << std::endl;
            } else if (outputMode && std::string_view{outputMode} == "lost") {
                std::cout << "GCP_VOICE_EVENT:connection_lost" << std::endl;
            }
            const auto *mode = std::getenv("GCP_FIXTURE_VOICE_OUTPUT");
            if (mode && std::string_view{mode} == "oversized")
                std::cout << std::string(16384, 'x') << std::flush;
        }
    }
    std::cout << "GCP_VOICE_EVENT:disconnected" << std::endl;
}
