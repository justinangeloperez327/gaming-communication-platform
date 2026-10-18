#include "voice_control.h"
#include <chrono>
#include <iostream>
#include <string>
#include <stdexcept>

using gcp::transport::VoiceControl;
using namespace std::chrono_literals;

int main() {
    const VoiceControl::TimePoint start{};
    auto check = [](bool condition) {
        if (!condition) throw std::runtime_error("voice control regression");
    };
    try {
        VoiceControl voice{true, true, false, start};
        check(!voice.transmit(start, false, true)); // starts muted before publication
        voice.receive("unmu", start);
        check(!voice.transmit(start, false, true));
        voice.receive("te\nping\n", start);
        check(voice.transmit(start, false, true));
        voice.receive("mute\n", start);
        check(voice.takeMuteRequest());
        check(!voice.takeMuteRequest());
        voice.receive("mute\n", start); // repeated mute still needs an SDK acknowledgment
        check(voice.takeMuteRequest());
        check(!voice.transmit(start, true, true));
        voice.receive("mode_ptt\nunmute\n", start);
        check(voice.takeMuteRequest());
        check(!voice.transmit(start, true, true)); // held at arming
        check(!voice.transmit(start, false, true));
        check(voice.transmit(start, true, true));
        check(!voice.transmit(start, false, true));
        check(voice.transmit(start, true, true));
        check(!voice.transmit(start, true, false)); // reconnect resets key gate
        check(!voice.transmit(start, true, true));
        check(!voice.transmit(start, false, true));
        check(voice.transmit(start, true, true));
        voice.receive("mode_open\n", start);
        check(!voice.transmit(start, false, true)); // mode switch re-mutes
        voice.receive("unmute\n", start);
        check(voice.transmit(start + 4999ms, false, true));
        check(voice.stopped(start + 5s));
        check(!voice.transmit(start + 5s, false, true));

        VoiceControl heartbeat{true, true, false, start};
        heartbeat.receive("ping\nunmute\n", start + 4s);
        check(!heartbeat.stopped(start + 8s));
        check(heartbeat.stopped(start + 9s));
        heartbeat.receive("stop\nunmute\nping\n", start + 8s);
        check(!heartbeat.transmit(start + 8s, false, true));

        VoiceControl listen{false, true, false, start};
        listen.receive("unmute\nmode_ptt\nunmute\n", start);
        check(!listen.transmit(start, false, true));
        check(!listen.transmit(start, true, true));
        VoiceControl invalid{true, true, false, start};
        invalid.receive(std::string(33, 'x'), start);
        check(invalid.stopped(start));
        VoiceControl unknown{true, true, false, start};
        unknown.receive("unmute\nunknown\n", start);
        check(unknown.stopped(start));
        VoiceControl binary{true, true, false, start};
        binary.receive(std::string_view{"\0", 1}, start);
        check(binary.stopped(start));
        VoiceControl diagnostic{true, false, false, start};
        check(diagnostic.transmit(start + 1h, false, true));
        diagnostic.stop();
        check(!diagnostic.transmit(start, false, true));
        // No five-minute duration policy is hidden in managed voice controls.
        VoiceControl long_call{true, true, false, start};
        for (int second = 0; second <= 43200; ++second)
            long_call.receive("ping\nunmute\n", start + std::chrono::seconds{second});
        check(long_call.transmit(start + 12h, false, true));
        std::cout << "Voice control policy tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
