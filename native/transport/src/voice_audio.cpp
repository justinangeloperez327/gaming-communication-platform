#include "voice_audio.h"

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace gcp::voice {
namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT hr, const char* message) {
    if (FAILED(hr)) throw std::runtime_error(message);
}
class Apartment {
public:
    Apartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(hr, "WASAPI requires a multithreaded COM apartment");
        active_ = true;
    }
    ~Apartment() { if (active_) CoUninitialize(); }
private:
    bool active_ = false;
};
struct HandleCloser {
    void operator()(void* h) const noexcept {
        if (h) CloseHandle(static_cast<HANDLE>(h));
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> devices;
    require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(devices.GetAddressOf())), "Failed to enumerate WASAPI endpoints");
    return devices;
}
ComPtr<IMMDevice> pick(IMMDeviceEnumerator* e, EDataFlow direction, int index) {
    ComPtr<IMMDevice> device;
    if (index == -1) {
        require(e->GetDefaultAudioEndpoint(direction, eCommunications, device.GetAddressOf()),
            "No default communications audio endpoint");
        return device;
    }
    ComPtr<IMMDeviceCollection> collection;
    require(e->EnumAudioEndpoints(direction, DEVICE_STATE_ACTIVE,
                                  collection.GetAddressOf()), "Cannot list audio endpoints");
    UINT count = 0;
    require(collection->GetCount(&count), "Cannot count audio endpoints");
    if (index < 0 || static_cast<UINT>(index) >= count)
        throw std::invalid_argument("Invalid audio device index");
    require(collection->Item(static_cast<UINT>(index), device.GetAddressOf()),
            "Cannot select audio endpoint");
    return device;
}
WAVEFORMATEX pcm() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = kChannels * 2;
    format.nAvgBytesPerSec = kRate * format.nBlockAlign;
    return format;
}
class Stream {
public:
    Stream(EDataFlow flow, int index) {
        auto e = enumerator();
        auto dev = pick(e.Get(), flow, index);
        require(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())), "Cannot activate audio stream");
        const auto fmt = pcm();
        // Explicit 48kHz mono PCM16. Windows shared-mode conversion is
        // requested; unsupported endpoints fail rather than reinterpret bytes.
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        require(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &fmt, nullptr),
            "Audio endpoint rejected 48kHz PCM16 shared-mode stream");
        event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event_) throw std::runtime_error("Failed to create WASAPI event");
        require(client_->SetEventHandle(event_.get()), "Failed to set WASAPI event");
    }
    IAudioClient* client() const { return client_.Get(); }
    HANDLE event() const { return static_cast<HANDLE>(event_.get()); }
    void start() { require(client_->Start(), "WASAPI stream start failed"); started_ = true; }
    ~Stream() { if (started_) (void)client_->Stop(); }
private:
    ComPtr<IAudioClient> client_;
    Handle event_;
    bool started_ = false;
};
void list_direction(IMMDeviceEnumerator* e, EDataFlow direction, const wchar_t* label) {
    ComPtr<IMMDeviceCollection> collection;
    require(e->EnumAudioEndpoints(direction, DEVICE_STATE_ACTIVE,
        collection.GetAddressOf()), "Audio device enumeration failed");
    UINT count = 0;
    require(collection->GetCount(&count), "Device count unavailable");
    std::wcout << label << L" (" << count << L")\n";
    for (UINT i = 0; i < count; ++i) {
        auto device = pick(e, direction, static_cast<int>(i));
        ComPtr<IPropertyStore> store;
        require(device->OpenPropertyStore(STGM_READ, store.GetAddressOf()),
            "Audio device property access failed");
        PROPVARIANT friendly;
        PropVariantInit(&friendly);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &friendly))
            && friendly.vt == VT_LPWSTR && friendly.pwszVal) {
            std::wcout << L"  [" << i << L"] " << friendly.pwszVal << L'\n';
        } else {
            std::wcout << L"  [" << i << L"] unnamed\n";
        }
        PropVariantClear(&friendly);
    }
}
} // namespace

void PlaybackQueue::push(std::span<const std::int16_t> mono) {
    std::lock_guard lock(mutex_);
    if (mono.size() > kMaxBufferedSamples) return; // caller counts invalid drops
    for (const auto sample : mono) {
        if (samples_.size() >= kMaxBufferedSamples) samples_.pop_front();
        samples_.push_back(sample);
    }
}
void PlaybackQueue::pop(std::span<std::int16_t> out, Counters& stats) {
    std::lock_guard lock(mutex_);
    for (auto& sample : out) {
        if (samples_.empty()) {
            sample = 0;
            ++stats.silence;
        } else {
            sample = samples_.front();
            samples_.pop_front();
        }
    }
}
std::size_t PlaybackQueue::size() const {
    std::lock_guard lock(mutex_);
    return samples_.size();
}
bool queue_self_test() {
    Counters stats;
    PlaybackQueue queue;
    std::array<std::int16_t, 4> samples{100, -10, 8, 42};
    queue.push(samples);
    std::array<std::int16_t, 6> out{};
    queue.pop(out, stats);
    if (out != std::array<std::int16_t, 6>{100, -10, 8, 42, 0, 0}) return false;
    if (stats.silence != 2 || queue.size() != 0) return false;
    std::vector<std::int16_t> capped(kMaxBufferedSamples + 123, 7);
    queue.push(std::span<const std::int16_t>(capped.data(), kMaxBufferedSamples));
    queue.push(samples);
    return queue.size() == kMaxBufferedSamples;
}
void list_devices() {
    Apartment com;
    auto e = enumerator();
    list_direction(e.Get(), eCapture, L"Input microphones");
    list_direction(e.Get(), eRender, L"Output speakers");
}

void capture_microphone(int index, std::atomic<bool>& running, Counters& stats,
        const std::function<void(std::span<const std::int16_t>)>& emit) {
    Apartment apartment;
    Stream stream(eCapture, index);
    ComPtr<IAudioCaptureClient> capture;
    require(stream.client()->GetService(IID_PPV_ARGS(capture.GetAddressOf())),
        "Unable to open microphone capture");
    std::array<std::int16_t, kBlockFrames> block{};
    std::size_t used = 0;
    stream.start();
    while (running.load()) {
        const auto waited = WaitForSingleObject(stream.event(), 100);
        if (waited == WAIT_TIMEOUT) continue;
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("Microphone event failed");
        UINT32 next = 0;
        require(capture->GetNextPacketSize(&next), "Microphone packet query failed");
        while (next && running.load()) {
            BYTE* bytes = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            require(capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr),
                "Microphone packet read failed");
            try {
                for (UINT32 i = 0; i < frames; ++i) {
                    std::int16_t sample = 0;
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && bytes) {
                        std::memcpy(&sample, bytes + (i * sizeof(sample)), sizeof(sample));
                    }
                    block[used++] = sample;
                    ++stats.captured;
                    if (used == kBlockFrames) {
                        emit(block);
                        ++stats.published;
                        used = 0;
                    }
                }
            } catch (...) {
                (void)capture->ReleaseBuffer(frames);
                throw;
            }
            require(capture->ReleaseBuffer(frames), "Microphone release failed");
            require(capture->GetNextPacketSize(&next), "Microphone next packet failed");
        }
    }
}
void play_speakers(int index, std::atomic<bool>& running, PlaybackQueue& queue,
                   Counters& stats) {
    Apartment apartment;
    Stream stream(eRender, index);
    ComPtr<IAudioRenderClient> render;
    require(stream.client()->GetService(IID_PPV_ARGS(render.GetAddressOf())),
        "Unable to open audio render client");
    UINT32 total_frames = 0;
    require(stream.client()->GetBufferSize(&total_frames), "Speaker buffer query failed");
    // Seed startup with silence; normal event-driven refills begin after Start.
    BYTE* buffer = nullptr;
    require(render->GetBuffer(total_frames, &buffer), "Speaker seed buffer failed");
    require(render->ReleaseBuffer(total_frames, AUDCLNT_BUFFERFLAGS_SILENT),
        "Speaker seed release failed");
    stream.start();
    while (running.load()) {
        const auto waited = WaitForSingleObject(stream.event(), 100);
        if (waited == WAIT_TIMEOUT) continue;
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("Speaker event failed");
        UINT32 padding = 0;
        require(stream.client()->GetCurrentPadding(&padding), "Speaker padding query failed");
        if (padding >= total_frames) continue;
        const UINT32 available = total_frames - padding;
        require(render->GetBuffer(available, &buffer), "Speaker buffer acquisition failed");
        std::vector<std::int16_t> samples(available);
        queue.pop(samples, stats);
        std::memcpy(buffer, samples.data(), samples.size() * sizeof(std::int16_t));
        require(render->ReleaseBuffer(available, 0), "Speaker buffer release failed");
        stats.played += available;
    }
}
} // namespace gcp::voice
