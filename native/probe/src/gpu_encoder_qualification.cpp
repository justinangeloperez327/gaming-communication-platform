#include "gpu_encoder_qualification.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace gcp::probe {
namespace {
using Microsoft::WRL::ComPtr;

void require(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(
            std::string(what) + " (HRESULT 0x" +
            std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

struct MfScope {
    MfScope() { require(MFStartup(MF_VERSION), "MFStartup"); }
    ~MfScope() { MFShutdown(); }
    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
};

struct Activations {
    IMFActivate** entries = nullptr;
    UINT32 size = 0;
    ~Activations() {
        for (UINT32 i = 0; i < size; ++i) {
            if (entries[i]) entries[i]->Release();
        }
        CoTaskMemFree(entries);
    }
};

struct Device {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFSample> sample;

    explicit Device(const GpuH264Profile& profile) {
        ComPtr<ID3D11DeviceContext> context;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        require(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            d3d.GetAddressOf(), nullptr, context.GetAddressOf()),
            "Create hardware D3D11 video device");

        UINT token = 0;
        require(MFCreateDXGIDeviceManager(
            &token, manager.GetAddressOf()), "Create DXGI device manager");
        require(manager->ResetDevice(d3d.Get(), token),
                "Attach D3D11 device to DXGI manager");

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(profile.width);
        desc.Height = static_cast<UINT>(profile.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // A device-side NV12 surface, intentionally unpopulated. This
        // validates allocation and sample wrapping, NOT color conversion.
        require(d3d->CreateTexture2D(&desc, nullptr, nv12.GetAddressOf()),
                "Allocate hardware NV12 texture");

        ComPtr<IMFMediaBuffer> media_buffer;
        require(MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE,
            media_buffer.GetAddressOf()), "Wrap NV12 surface for Media Foundation");
        require(MFCreateSample(sample.GetAddressOf()), "Create DXGI video sample");
        require(sample->AddBuffer(media_buffer.Get()), "Attach NV12 DXGI buffer");
    }
};

ComPtr<IMFMediaType> video_type(const GUID& subtype,
                              const GpuH264Profile& profile,
                              bool compressed) {
    ComPtr<IMFMediaType> type;
    require(MFCreateMediaType(type.GetAddressOf()), "Create media type");
    require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
            "Set major media type");
    require(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set video subtype");
    require(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
            "Set progressive video");
    require(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                               static_cast<UINT32>(profile.width),
                               static_cast<UINT32>(profile.height)),
            "Set frame size");
    require(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                static_cast<UINT32>(profile.fps), 1),
            "Set frame rate");
    require(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "Set pixel aspect ratio");
    if (compressed) {
        require(type->SetUINT32(
            MF_MT_AVG_BITRATE, static_cast<UINT32>(profile.bitrate_bps)),
            "Set target bitrate");
    }
    return type;
}

std::wstring friendly_name(IMFActivate* activate) {
    wchar_t* raw = nullptr;
    UINT32 length = 0;
    const auto status = activate->GetAllocatedString(
        MFT_FRIENDLY_NAME_Attribute, &raw, &length);
    std::wstring name = SUCCEEDED(status) && raw ? raw : L"<unnamed>";
    CoTaskMemFree(raw);
    return name;
}

bool qualify(IMFActivate* activate, const Device& device,
             const GpuH264Profile& profile) {
    ComPtr<IMFTransform> mft;
    const auto activation_status = activate->ActivateObject(
        IID_PPV_ARGS(mft.GetAddressOf()));
    if (FAILED(activation_status)) {
        std::wcout << L"  activation_failed=0x" << std::hex
                   << static_cast<unsigned long>(activation_status)
                   << std::dec << L"\n";
        return false;
    }
    struct Shutdown {
        IMFActivate* activate;
        ~Shutdown() { activate->ShutdownObject(); }
    } shutdown{activate};

    ComPtr<IMFAttributes> attributes;
    if (FAILED(mft->GetAttributes(attributes.GetAddressOf()))) {
        std::wcout << L"  no_mft_attributes\n";
        return false;
    }
    UINT32 d3d11_aware = 0;
    if (FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware))
        || d3d11_aware == 0) {
        std::wcout << L"  d3d11_aware=false (skipped)\n";
        return false;
    }

    UINT32 async = 0;
    if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
        // Mark transform as client-authorized for future event-driven use.
        // We do not call ProcessInput/ProcessOutput in this preflight probe.
        const auto result = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(result)) {
            std::wcout << L"  async_unlock_failed\n";
            return false;
        }
    }

    DWORD input_count = 0, output_count = 0;
    if (FAILED(mft->GetStreamCount(&input_count, &output_count))
        || input_count != 1 || output_count != 1) {
        std::wcout << L"  unsupported_stream_layout\n";
        return false;
    }
    DWORD input_id = 0, output_id = 0;
    const auto ids_status = mft->GetStreamIDs(1, &input_id, 1, &output_id);
    if (ids_status != E_NOTIMPL && FAILED(ids_status)) {
        std::wcout << L"  stream_id_query_failed\n";
        return false;
    }

    const auto manager_status = mft->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(device.manager.Get()));
    if (FAILED(manager_status)) {
        std::wcout << L"  device_manager_rejected=0x" << std::hex
                   << static_cast<unsigned long>(manager_status)
                   << std::dec << L"\n";
        return false;
    }

    auto output = video_type(MFVideoFormat_H264, profile, true);
    auto input = video_type(MFVideoFormat_NV12, profile, false);
    auto output_status = mft->SetOutputType(output_id, output.Get(), 0);
    auto input_status = SUCCEEDED(output_status)
        ? mft->SetInputType(input_id, input.Get(), 0)
        : output_status;
    if (FAILED(output_status) || FAILED(input_status)) {
        std::wcout << L"  format_negotiation_failed output=0x"
                   << std::hex << static_cast<unsigned long>(output_status)
                   << L" input=0x" << static_cast<unsigned long>(input_status)
                   << std::dec << L"\n";
        return false;
    }

    MFT_OUTPUT_STREAM_INFO output_info{};
    if (FAILED(mft->GetOutputStreamInfo(output_id, &output_info))) {
        std::wcout << L"  output_stream_info_failed\n";
        return false;
    }

    std::wcout << L"  qualified=true"
               << L" async=" << (async ? L"yes" : L"no")
               << L" gpu_surface=NV12"
               << L" codec=H264"
               << L" output_buffer_bytes_minimum=" << output_info.cbSize
               << L" output_sample_provider="
               << ((output_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
                       ? L"encoder" : L"caller") << L"\n";
    // We only negotiate and attach a device: NOT proof that one frame
    // was accepted or encoded, or that the resulting codec is compatible.
    return true;
}
} // namespace

bool valid_gpu_h264_profile(const GpuH264Profile& profile) {
    return profile.width >= 160 && profile.width <= 1920 &&
           (profile.width % 2) == 0 &&
           profile.height >= 90 && profile.height <= 1080 &&
           (profile.height % 2) == 0 &&
           profile.fps >= 1 && profile.fps <= 60 &&
           profile.bitrate_bps >= 100'000 &&
           profile.bitrate_bps <= 16'000'000;
}

int qualify_gpu_h264(const GpuH264Profile& profile) {
    if (!valid_gpu_h264_profile(profile))
        throw std::invalid_argument("Invalid GPU H.264 profile");

    MfScope media;
    Device device(profile);
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    Activations activations;
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    require(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                      nullptr, &output, &activations.entries,
                      &activations.size), "Enumerate hardware H264 MFTs");

    std::wcout << L"Hardware-class H.264 MFT candidates: "
               << activations.size << L"\n";
    UINT32 ready = 0;
    for (UINT32 i = 0; i < activations.size; ++i) {
        std::wcout << L"Candidate " << i << L": "
                   << friendly_name(activations.entries[i]) << L"\n";
        if (qualify(activations.entries[i], device, profile)) ++ready;
    }
    std::wcout << L"GPU_NV12_H264_QUALIFICATION summary"
               << L" candidates=" << activations.size
               << L" negotiated=" << ready
               << L" encoded_frames=0"
               << L" hardware_encoding_proven=false"
               << L"\n";
    return ready > 0 ? 0 : 2;
}
} // namespace gcp::probe
