#include "encoder_capabilities.h"

#include <Windows.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace gcp::probe {
namespace {
using Microsoft::WRL::ComPtr;

void check(HRESULT status, const char* operation) {
    if (FAILED(status)) {
        throw std::runtime_error(operation);
    }
}

struct MediaFoundationSession {
    MediaFoundationSession() { check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup failed"); }
    ~MediaFoundationSession() { MFShutdown(); }
    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;
};

struct MftList {
    IMFActivate** entries = nullptr;
    UINT32 count = 0;
    ~MftList() {
        for (UINT32 i = 0; i < count; ++i) {
            if (entries[i]) entries[i]->Release();
        }
        CoTaskMemFree(entries);
    }
    MftList(const MftList&) = delete;
    MftList& operator=(const MftList&) = delete;
    MftList() = default;
};

void print_codec(const wchar_t* label, const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, subtype};
    MftList candidates;
    // Includes both software and hardware MFTs. The flags request candidate
    // discovery, not validated real encoder selection.
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT |
                         MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_HARDWARE |
                         MFT_ENUM_FLAG_LOCALMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                    nullptr, &output_type, &candidates.entries, &candidates.count),
          "Unable to enumerate Media Foundation video encoders");

    std::wcout << label << L": " << candidates.count
               << L" registered candidate(s) (software/hardware mixed)\n";
    for (UINT32 i = 0; i < candidates.count; ++i) {
        wchar_t* friendly = nullptr;
        UINT32 length = 0;
        const HRESULT result = candidates.entries[i]->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &friendly, &length);
        if (SUCCEEDED(result) && friendly) {
            std::wcout << L"  - " << friendly << L"\n";
        } else {
            std::wcout << L"  - <unnamed encoder>\n";
        }
        CoTaskMemFree(friendly);
    }
}
}

void list_gpu_adapters() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),
          "DXGI adapter enumeration unavailable");
    std::wcout << L"DXGI adapters (installed graphics devices)\n";
    UINT i = 0;
    while (true) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(i, adapter.GetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND) break;
        check(result, "Unable to access DXGI adapter");
        DXGI_ADAPTER_DESC1 details{};
        check(adapter->GetDesc1(&details), "Unable to read adapter details");

        std::wcout << L"  [" << i << L"] " << details.Description
                   << L"  vendor=0x" << std::hex << details.VendorId
                   << L"  device=0x" << details.DeviceId << std::dec
                   << L"  dedicated_vram_mib="
                   << (details.DedicatedVideoMemory / (1024u * 1024u))
                   << L"  shared_mib="
                   << (details.SharedSystemMemory / (1024u * 1024u))
                   << L"  software=" << (bool(details.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L"yes" : L"no")
                   << L"\n";
        ++i;
    }
    std::wcout << L"Adapter inventory is not proof of a usable GPU encoder.\n";
}

void list_video_encoder_candidates() {
    MediaFoundationSession mf;
    std::wcout << L"Registered Media Foundation video encoders\n";
    print_codec(L"H.264", MFVideoFormat_H264);
    print_codec(L"HEVC", MFVideoFormat_HEVC);
    std::wcout << L"Enumeration only: does not establish GPU hardware selection,"
               << L" codec negotiation, resolution, rate, or LiveKit integration.\n";
}
} // namespace gcp::probe
