// Offline control-protocol fixture. Never initializes LiveKit or audio devices.
#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <chrono>
#include <thread>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "devices") {
        const auto *mode = std::getenv("GCP_FIXTURE_DEVICE_SCAN");
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
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--desktop-control") managed = true;
        if (argument == "--publish-mic") microphone = true;
        if (argument == "--seconds" && index + 1 < argc &&
            std::string_view{argv[index + 1]} == "0") unlimited = true;
    }
    if (!managed || !unlimited) return 2;
    std::cout << "GCP_VOICE_EVENT:connected\nGCP_VOICE_EVENT:muted" << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "stop") break;
        if (command == "mute" || command == "mode_ptt" || command == "mode_open")
            std::cout << "GCP_VOICE_EVENT:muted" << std::endl;
        else if (command == "unmute" && microphone)
            std::cout << "GCP_VOICE_EVENT:unmuted" << std::endl;
    }
    std::cout << "GCP_VOICE_EVENT:disconnected" << std::endl;
}
