#include "livekit_url.h"
// Group 27: explicit-consent LiveKit native Windows voice diagnostic.
// Uses the SDK's platform Audio Device Module for microphone, echo control,
// and default speaker playout. No raw audio data is logged or stored.
#include "telemetry.h"
#include "voice_control.h"
#include <Windows.h>
#include <array>

#include "livekit/livekit.h"
#include "livekit/local_audio_track.h"
#include "livekit/platform_audio.h"
#include "livekit/room_delegate.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> running{true};
std::mutex output_mutex;

// SDK callbacks and the session loop run on different threads. Keep each
// protocol line intact so a reconnect cannot splice a mute acknowledgment.
void voice_event(std::string_view event) {
    const std::scoped_lock lock(output_mutex);
    std::cout << "GCP_VOICE_EVENT:" << event << std::endl;
}

void interrupted(int) {
    running.store(false);
}

struct Options {
    enum class Mode { Help, SelfTest, Devices, Join };
    Mode mode = Mode::Help;
    int seconds = 30;
    int mute_after = -1;
    bool publish_mic = false;
    bool consent = false;
    bool desktop_control = false;
    bool push_to_talk = false;
    std::string input_id;
    std::string output_id;
};

bool parse_number(std::string_view text, int& result) {
    const auto [end, error] = std::from_chars(text.data(),
        text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size();
}

Options parse(int argc, char** argv) {
    Options o;
    if (argc < 2) return o;
    const std::string_view action(argv[1]);
    if (action == "--help" || action == "help") {
        if (argc != 2) throw std::invalid_argument("help takes no options");
        return o;
    }
    if (action == "--self-test") {
        if (argc != 2) throw std::invalid_argument("self-test takes no options");
        o.mode = Options::Mode::SelfTest;
        return o;
    }
    if (action == "devices") {
        if (argc != 2) throw std::invalid_argument("devices takes no options");
        o.mode = Options::Mode::Devices;
        return o;
    }
    if (action != "join") throw std::invalid_argument("Unsupported voice action");
    o.mode = Options::Mode::Join;

    bool seconds_seen = false, mute_seen = false, mic_seen = false;
    bool consent_seen = false, input_seen = false, output_seen = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--desktop-control") {
            if (o.desktop_control) throw std::invalid_argument("Duplicate desktop control");
            o.desktop_control = true;
        } else if (flag == "--push-to-talk") {
            if (o.push_to_talk) throw std::invalid_argument("Duplicate push-to-talk");
            o.push_to_talk = true;
        } else if (flag == "--publish-mic") {
            if (mic_seen) throw std::invalid_argument("Duplicate --publish-mic");
            mic_seen = true;
            o.publish_mic = true;
        } else {
            if (i + 1 >= argc) throw std::invalid_argument("Missing option value");
            const std::string_view value(argv[++i]);
            if (flag == "--seconds") {
                if (seconds_seen || !parse_number(value, o.seconds)
                    || o.seconds < 0 || o.seconds > 300)
                    throw std::invalid_argument("--seconds must be 0..300 (0 requires desktop control)");
                seconds_seen = true;
            } else if (flag == "--confirm-microphone") {
                if (consent_seen || value != "yes")
                    throw std::invalid_argument("Microphone consent must be yes");
                consent_seen = true;
                o.consent = true;
            } else if (flag == "--mute-after") {
                if (mute_seen || !parse_number(value, o.mute_after)
                    || o.mute_after < 1 || o.mute_after > 299)
                    throw std::invalid_argument("--mute-after must be 1..299");
                mute_seen = true;
            } else if (flag == "--input") {
                if (input_seen || value.empty() || value.size() > 512)
                    throw std::invalid_argument("Invalid --input ID");
                input_seen = true;
                o.input_id = value;
            } else if (flag == "--output") {
                if (output_seen || value.empty() || value.size() > 512)
                    throw std::invalid_argument("Invalid --output ID");
                output_seen = true;
                o.output_id = value;
            } else {
                throw std::invalid_argument("Unknown join option");
            }
        }
    }
    if (o.seconds == 0 && !o.desktop_control)
        throw std::invalid_argument("Unbounded calls require desktop control lease");
    if (o.push_to_talk && (!o.publish_mic || !o.desktop_control))
        throw std::invalid_argument("Push-to-talk requires managed microphone consent");
    if (o.publish_mic != o.consent)
        throw std::invalid_argument(
            "Microphone requires --publish-mic --confirm-microphone yes");
    if (mute_seen && (!o.publish_mic || o.desktop_control || o.mute_after >= o.seconds))
        throw std::invalid_argument("--mute-after requires mic and must precede end");
    if (input_seen && !o.publish_mic)
        throw std::invalid_argument("--input requires microphone publishing");
    return o;
}

void usage() {
    std::cout <<
        "GCP native LiveKit voice diagnostic (Windows, consent-gated)\n"
        "  --help\n"
        "  --self-test                     Offline parser/SDK smoke test\n"
        "  devices                         List input/output device IDs; no room join\n"
        "  join [--seconds 30] [--output DEVICE_ID]   Listen-only; microphone OFF\n"
        "  join --publish-mic --confirm-microphone yes [--seconds 30]\n"
        "       [--input DEVICE_ID] [--output DEVICE_ID] [--mute-after N]\n"
        "  Desktop only: --desktop-control --seconds 0 [--push-to-talk]\n"
        "  Control pipe commands: ping, mute, unmute, mode_ptt, mode_open, stop.\n"
        "  Five-second heartbeat lease; PTT holds F8 and starts muted.\n"
        "Requires LIVEKIT_URL and LIVEKIT_TOKEN only for join.\n"
        "Use microphone-scoped tokens from the authenticated Rust backend.\n"
        "No screen share, camera, audio file recording or voice activation.\n"
        "A successful room connection is NOT proof remote audio was heard.\n";
}

bool control_pipe_self_test();

int self_test() {
    char exe[] = "voice";
    char join[] = "join";
    char publish[] = "--publish-mic";
    char confirm[] = "--confirm-microphone";
    char yes[] = "yes";
    char seconds[] = "--seconds";
    char duration[] = "20";
    char mute[] = "--mute-after";
    char mute_time[] = "10";
    char* valid[] = {exe, join, publish, confirm, yes,
                     seconds, duration, mute, mute_time};
    const auto parsed = parse(9, valid);
    if (!parsed.publish_mic || !parsed.consent ||
        parsed.seconds != 20 || parsed.mute_after != 10) return 1;
    char* invalid[] = {exe, join, publish};
    try {
        (void)parse(3, invalid);
        return 2;
    } catch (const std::invalid_argument&) {}
    char* only_listen[] = {exe, join};
    if (parse(2, only_listen).publish_mic) return 3;
    char bad_time[] = "301";
    char* duration_invalid[] = {exe, join, seconds, bad_time};
    try {
        (void)parse(4, duration_invalid);
        return 4;
    } catch (const std::invalid_argument&) {}
    char zero[] = "0", managed[] = "--desktop-control", ptt[] = "--push-to-talk";
    char* unbounded[] = {exe, join, seconds, zero};
    try { (void)parse(4, unbounded); return 6; }
    catch (const std::invalid_argument&) {}
    char* controlled[] = {exe, join, managed, seconds, zero, publish, confirm, yes, ptt};
    const auto desktop = parse(9, controlled);
    if (!desktop.desktop_control || !desktop.push_to_talk || desktop.seconds != 0) return 7;
    char* listen_ptt[] = {exe, join, managed, ptt};
    try { (void)parse(4, listen_ptt); return 8; }
    catch (const std::invalid_argument&) {}
    char* duplicate_control[] = {exe, join, managed, managed};
    try { (void)parse(4, duplicate_control); return 9; }
    catch (const std::invalid_argument&) {}
    char input[] = "--input";
    char mic[] = "secret-device";
    char* bad_input[] = {exe, join, input, mic};
    try {
        (void)parse(4, bad_input);
        return 5;
    } catch (const std::invalid_argument&) {}
    if (!control_pipe_self_test()) return 10;
    livekit::initialize(livekit::LogLevel::Warn);
    livekit::shutdown();
    std::cout << "Voice CLI consent and LiveKit SDK self-test passed\n";
    return 0;
}

void print_devices(livekit::PlatformAudio& audio) {
    for (const auto& device : audio.recordingDevices())
        std::cout << "input [" << device.index << "] "
                  << device.name << " | id=" << device.id << '\n';
    for (const auto& device : audio.playoutDevices())
        std::cout << "output [" << device.index << "] "
                  << device.name << " | id=" << device.id << '\n';
}

void select_devices(livekit::PlatformAudio& audio, const Options& o) {
    if (!o.input_id.empty()) {
        const auto all = audio.recordingDevices();
        if (std::none_of(all.begin(), all.end(), [&](const auto& d) {
            return d.id == o.input_id;
        })) throw std::invalid_argument("Selected microphone device not found");
        audio.setRecordingDevice(o.input_id);
    }
    if (!o.output_id.empty()) {
        const auto all = audio.playoutDevices();
        if (std::none_of(all.begin(), all.end(), [&](const auto& d) {
            return d.id == o.output_id;
        })) throw std::invalid_argument("Selected speaker device not found");
        audio.setPlayoutDevice(o.output_id);
    }
}

struct VoiceEvents final : livekit::RoomDelegate {
    std::atomic<std::uint64_t> subscriptions{0};
    std::atomic<std::uint64_t> unsubscriptions{0};
    std::atomic<bool> network_lost{false};
    std::atomic<bool> reconnecting{false};
    std::atomic<std::uint64_t> interruptions{0};
    void onReconnecting(livekit::Room&, const livekit::ReconnectingEvent&) override {
        interruptions.fetch_add(1);
        reconnecting.store(true);
        voice_event("reconnecting");
    }
    void onReconnected(livekit::Room&, const livekit::ReconnectedEvent&) override {
        reconnecting.store(false);
        voice_event("reconnected");
    }
    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override {
        network_lost.store(true);
        voice_event("connection_lost");
    }

    void onTrackSubscribed(livekit::Room&,
            const livekit::TrackSubscribedEvent& e) override {
        if (e.track && e.track->kind() == livekit::TrackKind::KIND_AUDIO)
            subscriptions.fetch_add(1, std::memory_order_relaxed);
    }

    void onTrackUnsubscribed(livekit::Room&,
            const livekit::TrackUnsubscribedEvent& e) override {
        if (e.track && e.track->kind() == livekit::TrackKind::KIND_AUDIO)
            unsubscriptions.fetch_add(1, std::memory_order_relaxed);
    }
};

// QProcess stdin is a Windows anonymous pipe. Never block the audio/session
// loop waiting for input; parent EOF, malformed data and a stale lease fail closed.
void poll_control(gcp::transport::VoiceControl& control) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0;
    if (!input || input == INVALID_HANDLE_VALUE ||
        !PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        control.stop();
        return;
    }
    if (available == 0) return;
    std::array<char, 256> bytes{};
    DWORD used = 0;
    if (available > bytes.size() ||
        !ReadFile(input, bytes.data(), (std::min)(available, static_cast<DWORD>(bytes.size())),
                  &used, nullptr) || used == 0) {
        control.stop();
        return;
    }
    control.receive(std::string_view{bytes.data(), used}, Clock::now());
}

bool control_pipe_self_test() {
    struct Pipe {
        HANDLE read = INVALID_HANDLE_VALUE;
        HANDLE write = INVALID_HANDLE_VALUE;
        ~Pipe() {
            if (read != INVALID_HANDLE_VALUE) CloseHandle(read);
            if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
        }
        Pipe() = default;
        Pipe(const Pipe&) = delete;
        Pipe& operator=(const Pipe&) = delete;
    } pipe;
    if (!CreatePipe(&pipe.read, &pipe.write, nullptr, 0)) return false;
    struct RestoreInput {
        HANDLE original;
        ~RestoreInput() { SetStdHandle(STD_INPUT_HANDLE, original); }
    } restore{GetStdHandle(STD_INPUT_HANDLE)};
    if (!SetStdHandle(STD_INPUT_HANDLE, pipe.read)) return false;
    gcp::transport::VoiceControl control{true, true, false, Clock::now()};
    constexpr std::string_view commands{"ping\nunmute\n"};
    DWORD written = 0;
    if (!WriteFile(pipe.write, commands.data(), static_cast<DWORD>(commands.size()),
                   &written, nullptr) || written != commands.size()) return false;
    poll_control(control);
    if (!control.transmit(Clock::now(), false, true)) return false;
    CloseHandle(pipe.write);
    pipe.write = INVALID_HANDLE_VALUE;
    poll_control(control); // Actual Windows broken parent pipe, no SFU or audio.
    return control.stopped(Clock::now());
}

int join(const Options& o) {
    const char* raw_url = std::getenv("LIVEKIT_URL");
    const char* raw_token = std::getenv("LIVEKIT_TOKEN");
    if (!raw_url || !*raw_url || !raw_token || !*raw_token)
        throw std::invalid_argument("LIVEKIT_URL and LIVEKIT_TOKEN required");
    const std::string_view url(raw_url);
    if (!gcp::transport::permitted_livekit_url(url))
        throw std::invalid_argument("Only wss:// or loopback ws:// allowed");

    livekit::PlatformAudio audio;
    select_devices(audio, o);
    VoiceEvents events;
    livekit::Room room;
    room.setDelegate(&events);
    livekit::RoomOptions opts;
    opts.auto_subscribe = true;
    opts.connect_timeout = std::chrono::seconds(15);
    if (!room.connect(raw_url, raw_token, opts))
        throw std::runtime_error("Could not join LiveKit voice room");
    voice_event("connected");

    // Ensure microphone source and track both remain alive until disconnect.
    std::shared_ptr<livekit::PlatformAudioSource> mic_source;
    std::shared_ptr<livekit::LocalAudioTrack> mic_track;
    try {
        bool muted = o.desktop_control;
        if (o.publish_mic) {
            try {
                // Only reached after explicit CLI permission; no mic opened in
                // listen-only mode. WebRTC ADM supplies AEC, NS and AGC.
                mic_source = audio.createAudioSource();
                mic_track = livekit::LocalAudioTrack::createLocalAudioTrack(
                    "gcp-voice-microphone", mic_source);
                if (muted) mic_track->mute(); // before publication: no initial live burst
                livekit::TrackPublishOptions publication;
                publication.source = livekit::TrackSource::SOURCE_MICROPHONE;
                auto participant = room.localParticipant().lock();
                if (!participant)
                    throw std::runtime_error("Local participant is unavailable");
                participant->publishTrack(mic_track, publication);
                voice_event(muted ? "muted" : "unmuted");
            } catch (...) {
                voice_event("microphone_failed");
                throw;
            }
        } else {
            std::cerr << "LISTEN ONLY: microphone source was NOT created.\n";
        }
        const auto started = Clock::now();
        const auto before = gcp::probe::take_snapshot();
        gcp::transport::VoiceControl control{o.publish_mic, o.desktop_control,
                                              o.push_to_talk, started};
        auto interruption = events.interruptions.load();
        std::uint64_t peak_private_bytes = before.private_bytes;
        std::uint64_t sample_count = 0;
        auto next_perf_sample = Clock::now() + std::chrono::seconds(2);
        bool device_lost = false;
        auto next_device_check = Clock::now() + std::chrono::seconds(5);
        while (running.load() && !events.network_lost.load() &&
            (o.seconds == 0 || Clock::now() - started < std::chrono::seconds(o.seconds))) {
            if (o.desktop_control) {
                poll_control(control);
                const auto current_interruption = events.interruptions.load();
                if (interruption != current_interruption) {
                    // Discard any arming queued before this interruption.
                    control.interrupt();
                    interruption = current_interruption;
                    if (mic_track) {
                        mic_track->mute();
                        muted = true;
                        voice_event("rearm_required");
                    }
                }
                if (control.stopped(Clock::now())) break;
                if (control.takeMuteRequest() && mic_track) {
                    // Acknowledge explicit mute/mode requests even if already
                    // muted, and only after the SDK mute operation returns.
                    mic_track->mute();
                    muted = true;
                    voice_event("muted");
                }
                const bool key_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
                const bool transmit = control.transmit(Clock::now(), key_down,
                                                       !events.reconnecting.load());
                if (mic_track && transmit == muted) {
                    if (transmit) mic_track->unmute(); else mic_track->mute();
                    muted = !transmit;
                    voice_event(muted ? "muted" : "unmuted");
                }
            }
            // Only poll explicitly selected devices, at most once per five seconds.
            // A missing selected device fails closed rather than silently swapping
            // to a different microphone/output during an active voice call.
            if ((!o.input_id.empty() || !o.output_id.empty()) &&
                Clock::now() >= next_device_check) {
                next_device_check = Clock::now() + std::chrono::seconds(5);
                const auto present = [](const auto& devices, const std::string& id) {
                    return id.empty() || std::any_of(devices.begin(), devices.end(),
                        [&](const auto& d) { return d.id == id; });
                };
                try {
                    if ((!o.input_id.empty() && !present(audio.recordingDevices(), o.input_id)) ||
                        (!o.output_id.empty() && !present(audio.playoutDevices(), o.output_id))) {
                        device_lost = true;
                        voice_event("device_lost");
                        break;
                    }
                } catch (...) {
                    // An enumeration failure is not proof the device disappeared.
                    // Leave handling and SDK transport lifecycle remain functional.
                }
            }
            if (mic_track && !muted && o.mute_after > 0 &&
                Clock::now() - started >= std::chrono::seconds(o.mute_after)) {
                mic_track->mute();
                muted = true;
                std::cerr << "MICROPHONE MUTED: publication remains connected.\n";
            }
            if (Clock::now() >= next_perf_sample) {
                const auto sample = gcp::probe::take_snapshot();
                peak_private_bytes = std::max(peak_private_bytes, sample.private_bytes);
                ++sample_count;
                next_perf_sample = Clock::now() + std::chrono::seconds(2);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (mic_track && !muted) {
            // Explicit mute before teardown; no lingering outgoing audio.
            mic_track->mute();
        }
        const auto after = gcp::probe::take_snapshot();
        peak_private_bytes = std::max(peak_private_bytes, after.private_bytes);
        const auto machine_cpu = gcp::probe::total_machine_cpu_percent(
            before, after,
            std::chrono::duration<double>(Clock::now() - started).count(),
            gcp::probe::logical_processor_count());
        const auto elapsed =
            std::chrono::duration<double>(Clock::now() - started).count();
        const auto callbacks = events.subscriptions.load();
        const auto departures = events.unsubscriptions.load();
        {
            const std::scoped_lock lock(output_mutex);
            gcp::probe::print_metrics("livekit_voice_process_summary", elapsed,
                callbacks, departures, before, after);
            std::cout << "{\"probe\":\"livekit_voice_session\""
                      << ",\"desktop_managed\":" << (o.desktop_control ? "true" : "false")
                      << ",\"microphone_opted_in\":"
                      << (o.publish_mic ? "true" : "false")
                      << ",\"microphone_muted_at_end\":true"
                      << ",\"remote_audio_subscriptions\":" << callbacks
                      << ",\"remote_audio_unsubscriptions\":" << departures
                      << ",\"device_lost\":" << (device_lost ? "true" : "false")
                      << ",\"cpu_percent_total_machine\":" << machine_cpu
                      << ",\"private_bytes_start\":" << before.private_bytes
                      << ",\"private_bytes_end\":" << after.private_bytes
                      << ",\"peak_private_bytes_sampled\":" << peak_private_bytes
                      << ",\"memory_sample_count\":" << sample_count
                      << ",\"memory_sample_interval_seconds\":2"
                      << ",\"remote_audio_heard_verified\":false"
                      << ",\"physical_device_verified\":false"
                      << ",\"session_seconds\":" << std::fixed
                      << std::setprecision(3) << elapsed << "}\n";
        }
    } catch (...) {
        if (mic_track) {
            try { mic_track->mute(); } catch (...) {}
        }
        room.disconnect();
        throw;
    }
    room.disconnect();
    voice_event("disconnected");
    mic_track.reset();
    mic_source.reset();
    return 0; // A connected room does not imply the remote person heard it.
}

int execute(const Options& o) {
    livekit::initialize(livekit::LogLevel::Warn);
    try {
        int result = 0;
        if (o.mode == Options::Mode::Devices) {
            livekit::PlatformAudio audio;
            print_devices(audio);
        } else {
            result = join(o);
        }
        livekit::shutdown();
        return result;
    } catch (...) {
        livekit::shutdown();
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.mode == Options::Mode::Help) {
            usage();
            return 0;
        }
        if (options.mode == Options::Mode::SelfTest) return self_test();
        std::signal(SIGINT, interrupted);
        return execute(options);
    } catch (const std::exception&) {
        // Never include URL/token in exception messages returned by SDK;
        // replace connection errors with generic failure where possible.
        std::cerr << "Native LiveKit voice diagnostic failed; check configuration, consent and audio devices.\n";
        return 1;
    }
}
