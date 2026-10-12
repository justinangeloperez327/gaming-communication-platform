#include "capture_probe.h"
#include "telemetry.h"

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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gcp::probe {
namespace {
using Clock = std::chrono::steady_clock;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using Microsoft::WRL::ComPtr;

BOOL CALLBACK monitor_callback(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(context);
    monitors->push_back(monitor);
    return TRUE;
}

std::vector<HMONITOR> monitors() {
    std::vector<HMONITOR> result;
    if (!EnumDisplayMonitors(nullptr, nullptr, monitor_callback,
                             reinterpret_cast<LPARAM>(&result))) {
        throw std::runtime_error("EnumDisplayMonitors failed");
    }
    return result;
}

struct Counters {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
};
}

void list_monitors() {
    const auto devices = monitors();
    std::wcout << L"Monitors (" << devices.size() << L")\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(devices[i], &info)) {
            throw std::runtime_error("GetMonitorInfoW failed");
        }
        const auto width = info.rcMonitor.right - info.rcMonitor.left;
        const auto height = info.rcMonitor.bottom - info.rcMonitor.top;
        std::wcout << L"  [" << i << L"] " << info.szDevice << L" ("
                   << width << L"x" << height << L")\n";
    }
}

// GPU-backed capture proof: consume frames without CPU readback or disk writes.
// No streaming or WebRTC publication takes place in this diagnostic.
int run_monitor_probe(int index, int seconds) {
    if (!GraphicsCaptureSession::IsSupported()) {
        throw std::runtime_error("Windows Graphics Capture is not supported");
    }
    const auto available = monitors();
    if (index < 0 || static_cast<std::size_t>(index) >= available.size()) {
        throw std::runtime_error("Monitor index is invalid; run --list-monitors");
    }

    auto item_factory = winrt::get_activation_factory<
        GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(item_factory->CreateForMonitor(
        available[static_cast<std::size_t>(index)],
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)));

    ComPtr<ID3D11Device> d3d_device;
    ComPtr<ID3D11DeviceContext> d3d_context;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        d3d_device.GetAddressOf(), nullptr, d3d_context.GetAddressOf()));

    ComPtr<IDXGIDevice> dxgi_device;
    winrt::check_hresult(d3d_device.As(&dxgi_device));

    IDirect3DDevice winrt_device{nullptr};
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        dxgi_device.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(winrt_device))));

    auto size = item.Size();
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    auto session = pool.CreateCaptureSession(item);
    auto counters = std::make_shared<Counters>();

    const auto token = pool.FrameArrived([counters](
        Direct3D11CaptureFramePool const& sender, auto const&) {
        try {
            // Scope limits the frame lifetime; no lingering GPU textures.
            auto frame = sender.TryGetNextFrame();
            if (frame) {
                const auto content_size = frame.ContentSize();
                counters->width.store(content_size.Width);
                counters->height.store(content_size.Height);
                counters->frames.fetch_add(1);
                frame.Close();
            }
        } catch (...) {
            counters->errors.fetch_add(1);
        }
    });

    const auto start = Clock::now();
    const auto before = take_snapshot();
    try {
        session.StartCapture();
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } catch (...) {
        pool.FrameArrived(token);
        session.Close();
        pool.Close();
        throw;
    }

    pool.FrameArrived(token);
    session.Close();
    pool.Close();

    const auto after = take_snapshot();
    const auto duration = std::chrono::duration<double>(Clock::now() - start).count();
    const auto count = counters->frames.load();
    print_metrics("monitor", duration, count, counters->errors.load(),
                  before, after, counters->width.load(), counters->height.load());

    if (counters->errors.load() != 0) {
        std::cerr << "Frame callback errors occurred; capture requires investigation\n";
        return 3;
    }
    if (count == 0) {
        std::cerr << "No frames arrived; check screen activity and capture permissions\n";
        return 2;
    }
    return 0;
}

} // namespace gcp::probe
