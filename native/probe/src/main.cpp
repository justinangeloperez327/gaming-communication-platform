#include "audio_probe.h"
#include "capture_probe.h"
#include "telemetry.h"

#include <winrt/base.h>

#include <charconv>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
enum class Mode { help, monitors, audio, capture, microphone, test };

struct Options {
    Mode mode = Mode::help;
    int monitor = 0;
    int seconds = 5;
};

bool parse_int(std::string_view value, int& result) {
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size();
}

Options parse(int argc, char* argv[]) {
    Options options;
    if (argc <= 1) return options;

    const auto command = std::string_view(argv[1]);
    if (command == "--list-monitors") options.mode = Mode::monitors;
    else if (command == "--list-audio") options.mode = Mode::audio;
    else if (command == "--capture-monitor") options.mode = Mode::capture;
    else if (command == "--probe-mic") options.mode = Mode::microphone;
    else if (command == "--self-test") options.mode = Mode::test;
    else if (command == "--help") options.mode = Mode::help;
    else throw std::invalid_argument("Unknown command");

    int i = 2;
    if (options.mode == Mode::capture) {
        if (i >= argc || !parse_int(argv[i++], options.monitor) || options.monitor < 0) {
            throw std::invalid_argument("--capture-monitor needs a nonnegative monitor index");
        }
    }
    for (; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--seconds" && ++i < argc) {
            if (!parse_int(argv[i], options.seconds) ||
                options.seconds < 1 || options.seconds > 60) {
                throw std::invalid_argument("--seconds must be between 1 and 60");
            }
        } else {
            throw std::invalid_argument("Unexpected command argument");
        }
    }
    if (options.mode != Mode::capture && options.mode != Mode::microphone &&
        options.seconds != 5) {
        throw std::invalid_argument("--seconds only applies to active probes");
    }
    return options;
}

void help() {
    std::cout <<
        "Gaming Communication Platform - native media diagnostic\n"
        "  --list-monitors                 List display indices\n"
        "  --list-audio                    List active capture/render endpoints\n"
        "  --capture-monitor N [--seconds S]  Count captured GPU frames (1-60 s)\n"
        "  --probe-mic [--seconds S]       Count microphone audio frames (1-60 s)\n"
        "  --self-test                     Validate argument/telemetry primitives\n"
        "  --help                          Print usage\n"
        "Probe data stays local. No microphone or screen data is saved or uploaded.\n";
}

int self_test() {
    int value = 0;
    if (!parse_int("30", value) || value != 30 || parse_int("30abc", value)) return 1;
    gcp::probe::Snapshot from{0, 0};
    gcp::probe::Snapshot to{10'000'000, 0};
    const auto cpu = gcp::probe::total_machine_cpu_percent(from, to, 1.0, 4);
    if (cpu < 24.9 || cpu > 25.1) return 1;

    char name[] = "probe";
    char mode[] = "--capture-monitor";
    char monitor[] = "2";
    char flag[] = "--seconds";
    char seconds[] = "10";
    char* good_args[] = {name, mode, monitor, flag, seconds};
    const auto parsed = parse(5, good_args);
    if (parsed.mode != Mode::capture || parsed.monitor != 2 || parsed.seconds != 10) {
        return 1;
    }
    std::cout << "Native media probe self-test passed\n";
    return 0;
}
}

int main(int argc, char* argv[]) {
    try {
        const auto options = parse(argc, argv);
        if (options.mode == Mode::help) { help(); return 0; }
        if (options.mode == Mode::test) return self_test();

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        switch (options.mode) {
            case Mode::monitors: gcp::probe::list_monitors(); return 0;
            case Mode::audio: gcp::probe::list_audio_devices(); return 0;
            case Mode::capture:
                return gcp::probe::run_monitor_probe(options.monitor, options.seconds);
            case Mode::microphone:
                return gcp::probe::run_microphone_probe(options.seconds);
            default: break;
        }
        return 1;
    } catch (const winrt::hresult_error& e) {
        std::wcerr << L"Windows media API failed: " << e.message().c_str() << L"\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Media probe failed: " << e.what() << "\n";
        return 1;
    }
}
