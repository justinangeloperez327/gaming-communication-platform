#include "audio_probe.h"
#include "telemetry.h"

#include <Windows.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace gcp::probe {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

void check(HRESULT code, const char* operation) {
    if (FAILED(code)) {
        throw std::runtime_error(operation);
    }
}

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(result.GetAddressOf())),
          "Unable to create audio device enumerator");
    return result;
}

void print_devices(IMMDeviceEnumerator* devices, EDataFlow direction, const wchar_t* label) {
    ComPtr<IMMDeviceCollection> collection;
    check(devices->EnumAudioEndpoints(direction, DEVICE_STATE_ACTIVE,
                                      collection.GetAddressOf()),
          "Unable to enumerate audio devices");
    UINT count = 0;
    check(collection->GetCount(&count), "Unable to count audio devices");
    std::wcout << label << L" (" << count << L")\n";
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        check(collection->Item(i, device.GetAddressOf()),
              "Unable to access audio device");
        ComPtr<IPropertyStore> properties;
        check(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()),
              "Unable to read audio device properties");
        PROPVARIANT name;
        PropVariantInit(&name);
        const HRESULT status = properties->GetValue(PKEY_Device_FriendlyName, &name);
        if (SUCCEEDED(status) && name.vt == VT_LPWSTR && name.pwszVal) {
            std::wcout << L"  [" << i << L"] " << name.pwszVal << L"\n";
        } else {
            std::wcout << L"  [" << i << L"] <unnamed device>\n";
        }
        PropVariantClear(&name);
    }
}

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value) CloseHandle(static_cast<HANDLE>(value));
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct FormatCloser {
    void operator()(WAVEFORMATEX* value) const noexcept {
        CoTaskMemFree(value);
    }
};
using UniqueFormat = std::unique_ptr<WAVEFORMATEX, FormatCloser>;
}

void list_audio_devices() {
    const auto devices = enumerator();
    print_devices(devices.Get(), eCapture, L"Active microphones");
    print_devices(devices.Get(), eRender, L"Active playback devices");
}

// The probe reads and immediately releases WASAPI packets. It never records,
// stores, encodes, uploads, or plays captured microphone samples.
int run_microphone_probe(int seconds) {
    const auto devices = enumerator();
    ComPtr<IMMDevice> microphone;
    check(devices->GetDefaultAudioEndpoint(
              eCapture, eCommunications, microphone.GetAddressOf()),
          "No default communications microphone available");

    ComPtr<IAudioClient> audio;
    check(microphone->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, reinterpret_cast<void**>(audio.GetAddressOf())),
          "Unable to activate microphone");

    WAVEFORMATEX* raw_format = nullptr;
    check(audio->GetMixFormat(&raw_format), "Unable to obtain microphone format");
    UniqueFormat format(raw_format);

    check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            0, 0, format.get(), nullptr),
          "Unable to initialize shared-mode microphone stream");

    UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Unable to create microphone event");
    check(audio->SetEventHandle(event.get()), "Unable to configure microphone event");

    ComPtr<IAudioCaptureClient> capture;
    check(audio->GetService(IID_PPV_ARGS(capture.GetAddressOf())),
          "Unable to acquire microphone capture client");

    const auto start = Clock::now();
    const auto before = take_snapshot();
    check(audio->Start(), "Unable to start microphone capture");

    std::uint64_t frames = 0;
    std::uint64_t packets = 0;
    try {
        const auto deadline = start + std::chrono::seconds(seconds);
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
            const DWORD timeout = static_cast<DWORD>(std::max<std::int64_t>(
                1, std::min<std::int64_t>(250, remaining)));
            const DWORD event_status = WaitForSingleObject(event.get(), timeout);
            if (event_status == WAIT_FAILED) {
                throw std::runtime_error("Microphone event wait failed");
            }
            if (event_status != WAIT_OBJECT_0) continue;
            UINT32 available = 0;
            check(capture->GetNextPacketSize(&available), "Unable to query microphone packets");
            while (available != 0) {
                BYTE* data = nullptr;
                UINT32 count = 0;
                DWORD flags = 0;
                check(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr),
                      "Unable to access microphone buffer");
                // Explicitly do not inspect or persist any audio data.
                check(capture->ReleaseBuffer(count), "Unable to release microphone buffer");
                frames += count;
                ++packets;
                check(capture->GetNextPacketSize(&available),
                      "Unable to query next microphone packet");
            }
        }
    } catch (...) {
        audio->Stop();
        throw;
    }
    check(audio->Stop(), "Unable to stop microphone capture");
    const auto after = take_snapshot();
    const auto duration = std::chrono::duration<double>(Clock::now() - start).count();
    print_metrics("microphone", duration, frames, packets, before, after);
    return packets > 0 ? 0 : 2;
}

} // namespace gcp::probe
