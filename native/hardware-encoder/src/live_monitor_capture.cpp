#include "live_monitor_capture.h"
#include "gpu_color_converter.h"

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace gcp::media {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM user) {
    auto* output = reinterpret_cast<std::vector<HMONITOR>*>(user);
    output->push_back(monitor);
    return TRUE;
}

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::mutex video_context_mutex;
    GpuColorConverter converter;
    ComPtr<IMFSample> latest;
    std::uint64_t sequence = 0;
    const UINT width;
    const UINT height;
    const UINT fps;
    const Clock::time_point source_started = Clock::now();
    Clock::time_point last_sample{};
    std::atomic<bool> active{true};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> converted{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> failures{0};

    State(ID3D11Device* device, ID3D11DeviceContext* context,
          UINT w, UINT h, UINT rate, UINT source_w, UINT source_h)
        : converter(device, context, w, h, rate, source_w, source_h),
          width(source_w), height(source_h), fps(rate) {}

    void receive(const Direct3D11CaptureFramePool& pool) {
        if (!active.load()) return;
        // A free-threaded WGC frame callback can run on arbitrary workers.
        // Serialize video processor access, including GPU submission.
        std::lock_guard context_guard(video_context_mutex);
        auto frame = pool.TryGetNextFrame();
        if (!frame || !active.load()) return;
        received.fetch_add(1);

        const auto size = frame.ContentSize();
        if (size.Width != static_cast<int>(width) ||
            size.Height != static_cast<int>(height)) {
            failures.fetch_add(1);  // resize requires a new capture session
            active.store(false);
            changed.notify_all();
            return;
        }
        const auto now = Clock::now();
        const auto interval = std::chrono::nanoseconds(1'000'000'000LL / fps);
        if (last_sample != Clock::time_point{} && now - last_sample < interval) {
            dropped.fetch_add(1);
            return;
        }

        auto dxgi = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> surface;
        winrt::check_hresult(dxgi->GetInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(surface.GetAddressOf())));

        // Preserve real elapsed gaps on static screens and after dropped
        // frames. Frame-count timestamps compress those gaps into 1/fps and
        // drift away from the live RTP timeline. No sample time is mutated
        // after submission to the asynchronous encoder.
        const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - source_started).count() / 100;
        auto sample = converter.convert(
            surface.Get(), ts, 10'000'000LL / fps);
        {
            std::lock_guard lock(mutex);
            if (latest) dropped.fetch_add(1);
            latest = sample;
            ++sequence;
        }
        converted.fetch_add(1);
        last_sample = now;
        changed.notify_one();
    }
};
} // namespace

struct LiveMonitorCapture::Impl {
    std::shared_ptr<State> state;
    Direct3D11CaptureFramePool frame_pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    bool has_token = false;
    bool running = false;

    GraphicsCaptureItem item{nullptr};
    winrt::event_token closed_token{};
    bool has_closed_token = false;

    static GraphicsCaptureItem monitor_item(int index, UINT width, UINT height) {
        if (index < 0) throw std::invalid_argument("Invalid monitor index");
        std::vector<HMONITOR> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                                 reinterpret_cast<LPARAM>(&monitors)) ||
            static_cast<std::size_t>(index) >= monitors.size())
            throw std::invalid_argument("Monitor index out of range");
        auto factory = winrt::get_activation_factory<
            GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem selected{nullptr};
        winrt::check_hresult(factory->CreateForMonitor(
            monitors[static_cast<std::size_t>(index)],
            winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(selected)));
        const auto size = selected.Size();
        if (size.Width != static_cast<int>(width) ||
            size.Height != static_cast<int>(height))
            throw std::invalid_argument("Selected monitor dimensions differ from profile");
        return selected;
    }

    Impl(ID3D11Device* device, ID3D11DeviceContext* context,
         const GraphicsCaptureItem& selected, UINT width, UINT height, UINT fps,
         UINT source_width = 0, UINT source_height = 0)
        : item(selected) {
        if (!device || !context || !item || fps == 0 || fps > 60 ||
            width < 160 || height < 90 || width > 1920 || height > 1080 ||
            width % 2 || height % 2)
            throw std::invalid_argument("Invalid GPU capture profile");
        if (!GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows Graphics Capture not supported");
        const auto dimensions = item.Size();
        if ((source_width && dimensions.Width != static_cast<int>(source_width)) ||
            (source_height && dimensions.Height != static_cast<int>(source_height)))
            throw std::invalid_argument("Selected source resized after confirmation");
        if (dimensions.Width < 160 || dimensions.Height < 90 ||
            dimensions.Width > 1920 || dimensions.Height > 1080 ||
            (dimensions.Width + 1) / 2 * 2 != static_cast<int>(width) ||
            (dimensions.Height + 1) / 2 * 2 != static_cast<int>(height))
            throw std::invalid_argument("Selected source dimensions differ from profile");

        // Multi-thread access to the immediate D3D11 context is protected at
        // device level for interactions between WGC, VideoProcessor and MFT.
        ComPtr<ID3D11Multithread> multithread;
        winrt::check_hresult(context->QueryInterface(
            IID_PPV_ARGS(multithread.GetAddressOf())));
        multithread->SetMultithreadProtected(TRUE);
        state = std::make_shared<State>(device, context, width, height, fps,
                                              dimensions.Width, dimensions.Height);

        ComPtr<IDXGIDevice> dxgi_device;
        winrt::check_hresult(device->QueryInterface(
            IID_PPV_ARGS(dxgi_device.GetAddressOf())));
        IDirect3DDevice wrapped{nullptr};
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi_device.Get(),
            reinterpret_cast<IInspectable**>(winrt::put_abi(wrapped))));

        frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            wrapped, DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, dimensions);
        session = frame_pool.CreateCaptureSession(item);
        const auto shared = state;
        token = frame_pool.FrameArrived([shared](
            const Direct3D11CaptureFramePool& sender,
            const winrt::Windows::Foundation::IInspectable&) {
            if (!shared->active.load()) return;
            try {
                shared->receive(sender);
            } catch (...) {
                shared->failures.fetch_add(1);
                shared->active.store(false);
                shared->changed.notify_all();
            }
        });
        has_token = true;
        closed_token = item.Closed([shared](const auto&, const auto&) {
            shared->active.store(false);
            shared->changed.notify_all();
        });
        has_closed_token = true;
    }

    ~Impl() {
        if (state) {
            state->active.store(false);
            state->changed.notify_all();
        }
        // Close/unsubscribe can throw on device removal or OS source teardown.
        // Each operation must still be attempted; destructors must never throw.
        try { if (has_closed_token) item.Closed(closed_token); } catch (...) {}
        try { if (has_token) frame_pool.FrameArrived(token); } catch (...) {}
        try { if (session) session.Close(); } catch (...) {}
        try { if (frame_pool) frame_pool.Close(); } catch (...) {}
        if (state) {
            // An already-running free-threaded conversion owns shared State.
            // Wait for it before returning to MFShutdown/device teardown.
            std::lock_guard context_guard(state->video_context_mutex);
            std::lock_guard sample_guard(state->mutex);
            state->latest.Reset();
        }
    }
};

LiveMonitorCapture::LiveMonitorCapture(
    ID3D11Device* device, ID3D11DeviceContext* context,
    int monitor_index, UINT width, UINT height, UINT fps)
    : impl_(std::make_unique<Impl>(
          device, context, Impl::monitor_item(monitor_index, width, height),
          width, height, fps)) {}

LiveMonitorCapture::LiveMonitorCapture(
    ID3D11Device* device, ID3D11DeviceContext* context,
    const GraphicsCaptureItem& item, UINT width, UINT height, UINT fps,
    UINT source_width, UINT source_height)
    : impl_(std::make_unique<Impl>(device, context, item, width, height, fps,
                                   source_width, source_height)) {}

LiveMonitorCapture::~LiveMonitorCapture() = default;

void LiveMonitorCapture::start() {
    if (impl_->running) throw std::logic_error("Monitor capture already active");
    impl_->session.StartCapture();
    impl_->running = true;
}

bool LiveMonitorCapture::take_next(
    std::uint64_t& previous_sequence, ComPtr<IMFSample>& sample,
    DWORD timeout_ms) {
    const auto state = impl_->state;
    std::unique_lock lock(state->mutex);
    if (!state->changed.wait_for(
            lock, std::chrono::milliseconds(timeout_ms), [&] {
                return state->sequence != previous_sequence ||
                       !state->active.load();
            })) return false;
    if (!state->active.load() || !state->latest ||
        state->sequence == previous_sequence) return false;
    sample = std::move(state->latest);
    previous_sequence = state->sequence;
    return static_cast<bool>(sample);
}

std::uint64_t LiveMonitorCapture::received() const {
    return impl_->state->received.load();
}
std::uint64_t LiveMonitorCapture::converted() const {
    return impl_->state->converted.load();
}
std::uint64_t LiveMonitorCapture::dropped() const {
    return impl_->state->dropped.load();
}
std::uint64_t LiveMonitorCapture::failures() const {
    return impl_->state->failures.load();
}

bool LiveMonitorCapture::healthy() const {
    return impl_->state->active.load() &&
           impl_->state->failures.load() == 0;
}
} // namespace gcp::media
