// Offline control-protocol fixture. Never initializes LiveKit or audio devices.
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
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
