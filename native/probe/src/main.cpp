#include "audio_probe.h"
#include "encoder_capabilities.h"
#include "copy_benchmark.h"
#include "h264_encode_benchmark.h"
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
enum class Mode { help, monitors, audio, capture, microphone, gpus, encoders, hardware_h264, copy, encode, test };

struct Options {
    Mode mode = Mode::help;
    int monitor = 0;
    int seconds = 5;
    int width = 640;
    int height = 360;
    int iterations = 64;
    int fps = 30;
    int frames = 30;
    int bitrate = 1'500'000;
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
    else if (command == "--list-gpus") options.mode = Mode::gpus;
    else if (command == "--list-encoders") options.mode = Mode::encoders;
    else if (command == "--probe-hardware-h264") options.mode = Mode::hardware_h264;
    else if (command == "--benchmark-copy") options.mode = Mode::copy;
    else if (command == "--benchmark-h264") options.mode = Mode::encode;
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
        const std::string_view option = argv[i];
        if (option == "--seconds") {
            if (++i == argc || !parse_int(argv[i], options.seconds) ||
                options.seconds < 1 || options.seconds > 60)
                throw std::invalid_argument("--seconds must be 1..60");
        } else if ((options.mode == Mode::copy || options.mode == Mode::encode) && option == "--width") {
            if (++i == argc || !parse_int(argv[i], options.width) ||
                options.width < 160 || options.width > 3840)
                throw std::invalid_argument("--width must be 160..3840");
        } else if ((options.mode == Mode::copy || options.mode == Mode::encode) && option == "--height") {
            if (++i == argc || !parse_int(argv[i], options.height) ||
                options.height < 90 || options.height > 2160)
                throw std::invalid_argument("--height must be 90..2160");
        } else if (options.mode == Mode::copy && option == "--iterations") {
            if (++i == argc || !parse_int(argv[i], options.iterations) ||
                options.iterations < 1 || options.iterations > 1000)
                throw std::invalid_argument("--iterations must be 1..1000");
        } else if (options.mode == Mode::encode && option == "--fps") {
            if (++i == argc || !parse_int(argv[i], options.fps) ||
                options.fps < 1 || options.fps > 60)
                throw std::invalid_argument("--fps must be 1..60");
        } else if (options.mode == Mode::encode && option == "--frames") {
            if (++i == argc || !parse_int(argv[i], options.frames) ||
                options.frames < 1 || options.frames > 180)
                throw std::invalid_argument("--frames must be 1..180");
        } else if (options.mode == Mode::encode && option == "--bitrate") {
            if (++i == argc || !parse_int(argv[i], options.bitrate) ||
                options.bitrate < 100000 || options.bitrate > 16000000)
                throw std::invalid_argument("--bitrate must be 100000..16000000");
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
        "  --list-gpus                     Enumerate DXGI graphics adapters\n"
        "  --list-encoders                 Enumerate Media Foundation encoder candidates\n"
        "  --probe-hardware-h264          Activate hardware-class H.264 MFTs (no encode)\\n"
        "  --benchmark-copy [--width 640] [--height 360] [--iterations 64]\n"
        "  --benchmark-h264 [--width 640] [--height 360] [--fps 30]\n"
        "                   [--frames 30] [--bitrate 1500000] (synthetic NV12)\n"
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
    char copy_cmd[] = "--benchmark-copy";
    char width_flag[] = "--width";
    char wide[] = "1920";
    char* copy_args[] = {name, copy_cmd, width_flag, wide};
    const auto copy_test = parse(4, copy_args);
    if (copy_test.mode != Mode::copy || copy_test.width != 1920 ||
        copy_test.height != 360 || copy_test.iterations != 64) return 1;
    char bad_width[] = "-50";
    char* invalid[] = {name, copy_cmd, width_flag, bad_width};
    try {
        (void)parse(4, invalid);
        return 1;
    } catch (const std::invalid_argument&) {
    }
    char encode_mode[] = "--benchmark-h264";
    char frame_option[] = "--frames";
    char twelve[] = "12";
    char* valid_encode[] = {name, encode_mode, frame_option, twelve};
    const auto encoded = parse(4, valid_encode);
    if (encoded.mode != Mode::encode || encoded.frames != 12 ||
        encoded.fps != 30 || encoded.bitrate != 1'500'000) return 1;
    std::cout << "Native media probe self-test passed\n";
    return 0;
}
}

int main(int argc, char* argv[]) {
    try {
        const auto options = parse(argc, argv);
        if (options.mode == Mode::help) { help(); return 0; }
        if (options.mode == Mode::test) return self_test();

        if (options.mode == Mode::encode) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            return gcp::probe::benchmark_h264_encoding(
                options.width, options.height, options.fps,
                options.frames, options.bitrate);
        }

        if (options.mode == Mode::copy)
            return gcp::probe::run_copy_benchmark(
                options.width, options.height, options.iterations);

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        switch (options.mode) {
            case Mode::monitors: gcp::probe::list_monitors(); return 0;
            case Mode::audio: gcp::probe::list_audio_devices(); return 0;
            case Mode::gpus: gcp::probe::list_gpu_adapters(); return 0;
            case Mode::encoders: gcp::probe::list_video_encoder_candidates(); return 0;
            case Mode::hardware_h264: return gcp::probe::probe_hardware_h264_activation();
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
