#include "screen_capture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace gcp::transport {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

BOOL CALLBACK enumerate_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* devices = reinterpret_cast<std::vector<HMONITOR>*>(context);
    devices->push_back(monitor);
    return TRUE;
}

struct CaptureState {
    std::mutex graphics_mutex;
    std::mutex frame_mutex;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<std::uint8_t> latest;
    std::uint64_t sequence = 0;
    const int width;
    const int height;
    const int max_fps;
    Clock::time_point last_sample{};
    std::atomic<bool> active{true};
    std::atomic<std::uint64_t> arrivals{0};
    std::atomic<std::uint64_t> copied{0};
    std::atomic<std::uint64_t> throttled{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> readback_microseconds{0};
    // Last 256 GPU staging/map/readback durations; no unbounded telemetry.
    std::array<std::uint64_t, 256> readback_samples{};
    std::uint64_t sample_count = 0;

    CaptureState(int w, int h, int fps) : width(w), height(h), max_fps(fps) {}

    void receive(const Direct3D11CaptureFramePool& pool) {
        // Free-threaded frame pool may run on different worker threads.
        // Serialize D3D11 immediate-context access and staging readback.
        std::lock_guard graphics_lock(graphics_mutex);
        if (!active.load()) return;

        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        arrivals.fetch_add(1);

        const auto now = Clock::now();
        const auto min_period = std::chrono::nanoseconds(1'000'000'000LL / max_fps);
        if (last_sample != Clock::time_point{} && now - last_sample < min_period) {
            throttled.fetch_add(1);
            return;
        }

        const auto size = frame.ContentSize();
        if (size.Width != width || size.Height != height) {
            // Never pass incompatible data to the fixed-size SDK video source.
            // Dynamic resize and frame-pool recreation are later milestones.
            rejected.fetch_add(1);
            return;
        }

        auto native_access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(native_access->GetInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(texture.GetAddressOf())));

        D3D11_TEXTURE2D_DESC source_desc{};
        texture->GetDesc(&source_desc);
        if (source_desc.Width != static_cast<UINT>(width) ||
            source_desc.Height != static_cast<UINT>(height) ||
            source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            source_desc.SampleDesc.Count != 1) {
            rejected.fetch_add(1);
            return;
        }

        const auto readback_start = Clock::now();
        context->CopyResource(staging.Get(), texture.Get());
        // Allocate before Map: allocation failure must not leave the GPU
        // staging resource mapped and poison all subsequent captures.
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto bytes_per_row = static_cast<std::size_t>(width) * 4;
        if (mapped.RowPitch < bytes_per_row) {
            context->Unmap(staging.Get(), 0);
            rejected.fetch_add(1);
            return;
        }
        for (int row = 0; row < height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * bytes_per_row,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(row) * mapped.RowPitch,
                        bytes_per_row);
        }
        context->Unmap(staging.Get(), 0);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - readback_start).count();

        {
            std::lock_guard frame_lock(frame_mutex);
            latest.swap(pixels);
            ++sequence;
        }
        const auto measurement = static_cast<std::uint64_t>(
            (std::max<std::int64_t>)(0, micros));
        readback_samples[static_cast<std::size_t>(
            sample_count % readback_samples.size())] = measurement;
        ++sample_count;
        readback_microseconds.fetch_add(measurement);
        copied.fetch_add(1);
        last_sample = now;
    }
};
} // namespace

struct ScreenCapture::Impl {
    std::shared_ptr<CaptureState> state;
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    bool subscribed = false;
    bool started = false;

    Impl(int monitor_index, int fps) {
        if (fps < 1 || fps > 60) {
            throw std::invalid_argument("Capture FPS must be within 1..60");
        }
        if (!GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows Graphics Capture is unavailable");
        }
        std::vector<HMONITOR> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, enumerate_monitor,
                                 reinterpret_cast<LPARAM>(&monitors))) {
            throw std::runtime_error("Could not enumerate monitors");
        }
        if (monitor_index < 0 ||
            static_cast<std::size_t>(monitor_index) >= monitors.size()) {
            throw std::invalid_argument("Invalid monitor index; use gcp-media-probe --list-monitors");
        }

        auto factory = winrt::get_activation_factory<
            GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForMonitor(
            monitors[static_cast<std::size_t>(monitor_index)],
            winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));

        const auto size = item.Size();
        // Bound raw BGRA allocations while validating transfer performance.
        if (size.Width <= 0 || size.Height <= 0 ||
            size.Width > 1920 || size.Height > 1080) {
            throw std::runtime_error(
                "This feasibility prototype accepts monitors up to 1920x1080; "
                "GPU scaling is not yet implemented");
        }

        state = std::make_shared<CaptureState>(size.Width, size.Height, fps);
        ComPtr<ID3D11Device> device;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        winrt::check_hresult(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(),
            nullptr, state->context.GetAddressOf()));

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(size.Width);
        desc.Height = static_cast<UINT>(size.Height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(device->CreateTexture2D(
            &desc, nullptr, state->staging.GetAddressOf()));

        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        IDirect3DDevice wrapped{nullptr};
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(wrapped))));

        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            wrapped, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        auto shared = state;
        token = pool.FrameArrived([shared](
            const Direct3D11CaptureFramePool& sender,
            const winrt::Windows::Foundation::IInspectable&) {
            if (!shared->active.load()) return;
            try {
                shared->receive(sender);
            } catch (...) {
                // Callback exceptions must not terminate the process.
                shared->failures.fetch_add(1);
            }
        });
        subscribed = true;
    }

    ~Impl() {
        state->active.store(false);
        if (subscribed) pool.FrameArrived(token);
        if (session) session.Close();
        if (pool) pool.Close();
    }
};

ScreenCapture::ScreenCapture(int monitor_index, int max_fps)
    : impl_(std::make_unique<Impl>(monitor_index, max_fps)) {}

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::start() {
    if (impl_->started) throw std::logic_error("Capture already started");
    impl_->session.StartCapture();
    impl_->started = true;
}

bool ScreenCapture::take_latest(std::uint64_t& previous_sequence, ScreenFrame& target) {
    const auto state = impl_->state;
    std::lock_guard lock(state->frame_mutex);
    if (state->sequence == previous_sequence) return false;
    // Exchange ownership of the latest CPU buffer instead of copying it.
    // The next frame replaces this slot; the consumer owns its pixel buffer.
    target.bgra.swap(state->latest);
    target.width = state->width;
    target.height = state->height;
    target.sequence = state->sequence;
    previous_sequence = state->sequence;
    return true;
}

ScreenCaptureStats ScreenCapture::stats() const {
    auto& s = *impl_->state;
    std::lock_guard lock(s.graphics_mutex);
    const auto used = static_cast<std::size_t>(
        (std::min<std::uint64_t>)(s.sample_count, s.readback_samples.size()));
    std::array<std::uint64_t, 256> sorted = s.readback_samples;
    std::sort(sorted.begin(), sorted.begin() + used);
    const auto p50 = used > 0 ? sorted[(used - 1) / 2] : 0;
    const auto p95 = used > 0 ? sorted[
        static_cast<std::size_t>((used - 1) * 0.95)] : 0;
    return {s.arrivals.load(), s.copied.load(), s.throttled.load(),
            s.rejected.load(), s.failures.load(), s.readback_microseconds.load(),
            p50, p95};
}

int ScreenCapture::width() const { return impl_->state->width; }
int ScreenCapture::height() const { return impl_->state->height; }

} // namespace gcp::transport
