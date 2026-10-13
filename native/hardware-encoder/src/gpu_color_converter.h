#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <wrl/client.h>

namespace gcp::media {

// Device-only BGRA -> NV12 conversion. The D3D11 video processor, textures,
// and Media Foundation samples share ONE hardware D3D11 device. No GPU -> CPU
// readback occurs here. Caller owns device/context synchronization.
class GpuColorConverter final {
public:
    GpuColorConverter(ID3D11Device* device, ID3D11DeviceContext* context,
                      UINT width, UINT height, UINT fps);

    Microsoft::WRL::ComPtr<IMFSample> convert(
        ID3D11Texture2D* bgra, LONGLONG timestamp_100ns,
        LONGLONG duration_100ns);

private:
    UINT width_;
    UINT height_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
};

} // namespace gcp::media
