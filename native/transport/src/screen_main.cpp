#include "screen_control.h"
#include "screen_h264_session.h"
#include "h264_access_unit.h"
#include "livekit/livekit.h"
#include "livekit/encoded_video_source.h"
#include <Windows.h>
#include <Shobjidl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>

namespace {
using gcp::transport::screen_now_ms;
using gcp::transport::ScreenControl;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
std::mutex output_mutex;
void event(const std::string& message) {
    const std::lock_guard lock(output_mutex);
    const auto line = "GCP_SCREEN " + message + "\n";
    DWORD written = 0;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(),
                   static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
        throw std::runtime_error("Parent pipe closed");
}
std::string base64(std::string_view text) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned bits = 0; int count = 0;
    for (const unsigned char c : text) {
        bits = (bits << 8) | c; count += 8;
        while (count >= 6) { count -= 6; out += alphabet[(bits >> count) & 63]; }
    }
    if (count) out += alphabet[(bits << (6 - count)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}
// Keep SDK/hardware diagnostics out of the bounded parent protocol and logs.
struct Discard final : std::streambuf { int overflow(int c) override { return c; } };
class Input final {
public:
    explicit Input(ScreenControl& control) : control_(control), reader_([this] { read(); }) {}
    ~Input() {
        cancelled_.store(true);
        if (reader_.joinable()) reader_.join();
    }
private:
    void read() {
        std::array<char, 1024> bytes{};
        std::string line;
        while (!cancelled_.load()) {
            DWORD available = 0;
            const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
            DWORD size = 0;
            if (!ReadFile(pipe, bytes.data(),
                          (std::min)(available, static_cast<DWORD>(bytes.size())), &size, nullptr) || !size) break;
            for (DWORD i = 0; i < size; ++i) {
                if (bytes[i] == '\n') {
                    if (!control_.accept(line, screen_now_ms())) { control_.stop(); return; }
                    line.clear();
                } else {
                    if (line.size() >= 12288 || bytes[i] == '\0' || bytes[i] == '\r') {
                        control_.stop(); return;
                    }
                    line += bytes[i];
                }
            }
        }
        control_.stop();
    }
    ScreenControl& control_;
    std::atomic<bool> cancelled_{false};
    std::thread reader_;
};
struct Events final : livekit::RoomDelegate {
    explicit Events(ScreenControl& value) : control(value) {}
    ScreenControl& control;
    void onReconnecting(livekit::Room&, const livekit::ReconnectingEvent&) override { control.stop(); }
    void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override { control.stop(); }
};
void publish(ScreenControl& control, const gcp::transport::ScreenGrant& grant,
             const GraphicsCaptureItem& item, int source_width, int source_height) {
    livekit::initialize(livekit::LogLevel::Off);
    struct Shutdown { ~Shutdown() { livekit::shutdown(); } } shutdown;
    Events events(control); // Must outlive Room, including asynchronous teardown.
    livekit::Room room;
    room.setDelegate(&events);
    livekit::RoomOptions options;
    options.auto_subscribe = false;
    options.join_retries = 0;
    options.connect_timeout = std::chrono::seconds(10);
    if (!control.alive(screen_now_ms())) return;
    if (!room.connect(grant.url, grant.token, options)) throw std::runtime_error("Connection failed");
    if (!control.alive(screen_now_ms())) return;
    auto participant = room.localParticipant().lock();
    if (!participant || participant->identity() != grant.identity)
        throw std::runtime_error("Identity mismatch");
    // The Qt client also validates the JWT's exact room and source-only grants.
    const auto size = item.Size();
    if (size.Width != source_width || size.Height != source_height)
        throw std::runtime_error("Selected source resized after selection");
    gcp::media::HardwareH264Profile profile;
    profile.width = (size.Width + 1) / 2 * 2;
    profile.height = (size.Height + 1) / 2 * 2;
    auto source = std::make_shared<livekit::EncodedVideoSource>(
        livekit::VideoCodec::H264, profile.width, profile.height);
    auto track = participant->publishVideoTrack(
        "gcp-screen", source, livekit::TrackSource::SOURCE_SCREENSHARE);
    if (!track) throw std::runtime_error("Publication failed");
    if (!control.alive(screen_now_ms())) return;
    event("connected");
    bool first_idr = false;
    std::int64_t previous = -1;
    const auto emit = [&](const gcp::media::HardwareH264AccessUnit& encoded) {
        if (!control.alive(screen_now_ms())) return false;
        auto checked = gcp::transport::verify_h264_access_unit(encoded.bytes, encoded.parameter_sets);
        if ((encoded.keyframe && !checked.keyframe) || encoded.timestamp_us < 0 ||
            encoded.timestamp_us < previous) throw std::runtime_error("Invalid H264 output");
        previous = encoded.timestamp_us;
        if (!first_idr && !checked.keyframe) return true;
        livekit::EncodedVideoSource::Frame frame;
        frame.data = std::move(checked.annexb);
        frame.timestamp_us = encoded.timestamp_us;
        frame.is_keyframe = checked.keyframe;
        if (!control.alive(screen_now_ms()) || !source->captureFrame(frame)) return false;
        if (!first_idr) { first_idr = true; event("sharing"); }
        return true;
    };
    const auto feedback = [&] {
        const auto value = source->takeFeedback();
        gcp::media::HardwareH264Feedback request;
        request.force_keyframe = value.keyframe_requested || !first_idr;
        if (value.rate_control && value.rate_control->target_bitrate_bps >= 100'000 &&
            value.rate_control->target_bitrate_bps <= 16'000'000)
            request.target_bitrate_bps = static_cast<std::uint32_t>(value.rate_control->target_bitrate_bps);
        return request;
    };
    const gcp::media::ScreenH264Session session{
        item, [&] { return control.alive(screen_now_ms()); }, [] { event("capturing"); },
        source_width, source_height};
    if (gcp::media::encode_screen_h264(profile, session, emit, feedback) != 0)
        throw std::runtime_error("Hardware capture unavailable");
    // Source/track references and GPU encoder are destroyed before Room/SDK.
}
GraphicsCaptureItem pick(ScreenControl& control) {
    using namespace winrt::Windows::Graphics::Capture;
    if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Capture unsupported");
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"GcpScreenPickerOwner";
    type.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == WM_CLOSE) {
            auto* state = reinterpret_cast<ScreenControl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (state) state->stop();
            DestroyWindow(window); return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    };
    if (!RegisterClassW(&type)) throw std::runtime_error("Picker class unavailable");
    const auto owner = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Choose what to share - GCP",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 420, 180,
        nullptr, nullptr, type.hInstance, nullptr);
    if (!owner) throw std::runtime_error("Picker owner unavailable");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } window{owner};
    SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&control));
    ShowWindow(owner, SW_SHOWNORMAL);
    SetForegroundWindow(owner);
    if (!SetTimer(owner, 1, 100, nullptr)) throw std::runtime_error("Picker timer unavailable");
    GraphicsCapturePicker picker;
    winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(owner));
    auto operation = picker.PickSingleItemAsync();
    const auto done = std::make_shared<std::atomic<bool>>(false);
    operation.Completed([done](const auto&, const auto&) { done->store(true); });
    while (!done->load() && control.alive(screen_now_ms())) {
        MSG message{};
        if (GetMessageW(&message, nullptr, 0, 0) <= 0) { control.stop(); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (!control.alive(screen_now_ms())) { operation.Cancel(); return nullptr; }
    return operation.GetResults();
}
int run() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto shared_control = std::make_shared<ScreenControl>(screen_now_ms());
    auto& control = *shared_control;
    Input input(control);
    // Enforce parent loss and grant expiry even while the SDK/driver blocks.
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        std::int64_t stopped_at = 0;
        while (!finished.load()) {
            const auto now = screen_now_ms();
            if (!control.alive(now)) {
                control.stop();
                if (!stopped_at) stopped_at = now;
                if (now - stopped_at > 2000) TerminateProcess(GetCurrentProcess(), 2);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    struct Finish {
        std::atomic<bool>& flag; std::thread& thread;
        ~Finish() { flag.store(true); thread.join(); }
    } finish{finished, watchdog};
    auto item = pick(control);
    if (!item) { event("cancelled"); return 0; }
    const auto size = item.Size();
    if (size.Width < 160 || size.Height < 90 || size.Width > 1920 || size.Height > 1080) {
        event("unsupported_size"); return 1;
    }
    auto title = winrt::to_string(item.DisplayName());
    if (title.empty() || title.size() > 1024) title = "Selected source";
    const auto closed = item.Closed([shared_control](const auto&, const auto&) { shared_control->stop(); });
    // Remove the callback while its control object still exists.
    struct Unsubscribe {
        GraphicsCaptureItem& item; winrt::event_token token;
        ~Unsubscribe() { try { item.Closed(token); } catch (...) {} }
    } unsubscribe{item, closed};
    control.selected();
    event("source " + base64(title) + " " + std::to_string(size.Width) + " " + std::to_string(size.Height));
    std::optional<gcp::transport::ScreenGrant> grant;
    while (control.alive(screen_now_ms()) && !(grant = control.take_grant()))
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (grant && control.alive(screen_now_ms())) {
        auto agile = winrt::agile_ref(item);
        std::atomic<bool> worker_done{false};
        std::thread worker([&control, &worker_done, agile, size, grant = std::move(*grant)] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                publish(control, grant, agile.get(), size.Width, size.Height);
            } catch (...) { control.stop(); try { event("failed"); } catch (...) {} }
            worker_done.store(true);
        });
        // Service the picker's STA until all WinRT/SDK resources have closed.
        while (!worker_done.load()) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        worker.join();
    }
    control.stop();
    event("stopped");
    return 0;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "GCP desktop screen publisher: --pick-source | --self-test | --help\n"
                     "Windows picker then explicit Qt consent and a bounded stdin grant are required.\n"
                     "SDR 160x90 through 1920x1080, hardware H264, 30 fps. No audio.\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        ScreenControl control(1000);
        if (!control.accept("ping", 1100) || !control.accept("stop", 1101) || control.alive(1102) ||
            !gcp::transport::h264_access_unit_self_test()) return 1;
        std::cout << "Screen control and H264 self-test passed; no picker, devices or network opened.\n";
        return 0;
    }
    if (argc != 2 || std::string_view(argv[1]) != "--pick-source") return 1;
    Discard discard;
    auto* old_out = std::cout.rdbuf(&discard);
    auto* old_err = std::cerr.rdbuf(&discard);
    int result = 1;
    try { result = run(); } catch (...) { try { event("failed"); } catch (...) {} }
    std::cout.rdbuf(old_out); std::cerr.rdbuf(old_err);
    return result;
}
