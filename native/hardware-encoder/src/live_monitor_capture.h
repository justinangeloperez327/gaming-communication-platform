#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <winrt/Windows.Graphics.Capture.h>

namespace gcp::media {

// Explicitly authorized source for GPU-backed Media Foundation samples.
// No CPU pixel readback, audio capture, file recording or networking.
// All capture resources share the encoder's D3D11 hardware device.
class LiveMonitorCapture final {
public:
    LiveMonitorCapture(ID3D11Device* device, ID3D11DeviceContext* context,
                       int monitor_index, UINT width, UINT height, UINT fps);
    LiveMonitorCapture(ID3D11Device* device, ID3D11DeviceContext* context,
                       const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item,
                       UINT width, UINT height, UINT fps);
    ~LiveMonitorCapture();

    LiveMonitorCapture(const LiveMonitorCapture&) = delete;
    LiveMonitorCapture& operator=(const LiveMonitorCapture&) = delete;

    void start();
    // Waits for one NEW GPU NV12 sample, bounded by timeout. Returns false
    // on timeout, resize, callback failure or source closure.
    bool take_next(std::uint64_t& previous_sequence,
                   Microsoft::WRL::ComPtr<IMFSample>& sample,
                   DWORD timeout_ms);
    std::uint64_t received() const;
    std::uint64_t converted() const;
    std::uint64_t dropped() const;
    std::uint64_t failures() const;
    bool healthy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gcp::media
