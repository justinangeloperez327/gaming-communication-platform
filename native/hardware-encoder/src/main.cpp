#include "gpu_color_converter.h"
#include "live_monitor_capture.h"
#include "hardware_h264_bridge.h"
#include "screen_h264_session.h"
#include "../../transport/src/frame_arrival_metrics.h"
#include "../../probe/src/telemetry.h"

#include <winrt/base.h>

#include <Windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <oaidl.h>
#include <wrl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <deque>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {
void require(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                 std::to_string(static_cast<unsigned long>(hr)));
    }
}

struct Options {
    int width = 640;
    int height = 360;
    int fps = 30;
    int frames = 15;
    int bitrate = 1'500'000;
    int timeout_seconds = 20;
    int duration_seconds = 0;
    bool use_bgra_gpu_conversion = false;
    bool use_monitor_capture = false;
    int monitor_index = -1;
    bool confirmed_monitor_capture = false;
};

bool read_number(std::string_view value, int& target) {
    auto [p, status] = std::from_chars(value.data(), value.data() + value.size(), target);
    return status == std::errc{} && p == value.data() + value.size();
}
bool valid(const Options& o) {
    return o.width >= 160 && o.width <= 1920 && o.width % 2 == 0 &&
           o.height >= 90 && o.height <= 1080 && o.height % 2 == 0 &&
           o.fps >= 1 && o.fps <= 60 && o.frames >= 1 && o.frames <= 18000 &&
           o.bitrate >= 100'000 && o.bitrate <= 16'000'000 &&
           o.timeout_seconds >= 5 && o.timeout_seconds <= 330 &&
           o.duration_seconds >= 0 && o.duration_seconds <= 300 &&
           (o.duration_seconds == 0 || (o.use_monitor_capture &&
                o.timeout_seconds >= o.duration_seconds + 5)) &&
           (o.use_monitor_capture
               ? (o.use_bgra_gpu_conversion && o.monitor_index >= 0 &&
                  o.confirmed_monitor_capture)
               : (o.monitor_index == -1 && !o.confirmed_monitor_capture));
}

Options parse(int argc, char* argv[]) {
    Options o;
    bool frames_explicit = false;
    bool timeout_explicit = false;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::invalid_argument("Missing flag value");
        const std::string_view flag = argv[i];
        if (flag == "--source") {
            const std::string_view input = argv[i+1];
            if (input == "nv12") {
                o.use_bgra_gpu_conversion = false;
                o.use_monitor_capture = false;
            } else if (input == "bgra") {
                o.use_bgra_gpu_conversion = true;
                o.use_monitor_capture = false;
            } else if (input == "monitor") {
                o.use_bgra_gpu_conversion = true;
                o.use_monitor_capture = true;
            } else throw std::invalid_argument(
                "--source must be nv12, bgra or monitor");
            continue;
        }
        if (flag == "--confirm-monitor-share") {
            if (std::string_view(argv[i+1]) != "yes")
                throw std::invalid_argument(
                    "--confirm-monitor-share must be exactly yes");
            o.confirmed_monitor_capture = true;
            continue;
        }
        int number = 0;
        if (!read_number(argv[i+1], number))
            throw std::invalid_argument("Numeric argument required");
        if (flag == "--width") o.width = number;
        else if (flag == "--height") o.height = number;
        else if (flag == "--fps") o.fps = number;
        else if (flag == "--frames") {
            o.frames = number;
            frames_explicit = true;
        }
        else if (flag == "--bitrate") o.bitrate = number;
        else if (flag == "--timeout") {
            o.timeout_seconds = number;
            timeout_explicit = true;
        }
        else if (flag == "--duration") o.duration_seconds = number;
        else if (flag == "--monitor") o.monitor_index = number;
        else throw std::invalid_argument("Unknown flag");
    }
    // Validate before multiplication/addition: CLI integers may be INT_MAX.
    if (o.fps < 1 || o.fps > 60 ||
        o.duration_seconds < 0 || o.duration_seconds > 300)
        throw std::invalid_argument("Invalid FPS or duration");
    if (o.duration_seconds > 0) {
        if (!frames_explicit) o.frames = o.fps * o.duration_seconds;
        if (!timeout_explicit) o.timeout_seconds = o.duration_seconds + 15;
    }
    if (!valid(o)) throw std::invalid_argument("Unsupported H.264 hardware profile");
    return o;
}

void usage() {
    std::cout <<
        "gcp-hardware-h264: asynchronous hardware-class MFT GPU test\n"
        "  --help / --self-test\n"
        "  --run [--width 640] [--height 360] [--fps 30] [--frames 15]\n"
        "        [--bitrate 1500000] [--timeout 20]\n"
        "        [--duration 1..300] (monitor only; auto timeout/duration)\n"
        "        [--source nv12|bgra|monitor]\n"
        "        [--monitor N --confirm-monitor-share yes] (monitor only)\n"
        "nv12/bgra generate SYNTHETIC frames. MONITOR accesses the selected\n"
        "display, with explicit consent. No files/network/audio.\n"
        "A physical GPU and live Windows session are required.\n"
        "BGRA input uses D3D11 video processing to convert to GPU NV12.\n"
        "Fails if no D3D11-aware hardware-class encoder emits H.264 output.\n";
}

// Never poll the MFT's ProcessInput/ProcessOutput loops. The SDK provides
// asynchronous readiness events, which we deliver through a Win32 event.
// The event queue is bounded to prevent runaway memory usage.
class EventPump final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IMFAsyncCallback> {
public:
    EventPump() {
        signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!signal_) throw std::runtime_error("CreateEvent failed");
    }
    ~EventPump() {
        stop();
        CloseHandle(signal_);
    }
    STDMETHOD(GetParameters)(DWORD*, DWORD*) override { return E_NOTIMPL; }

    void start(const ComPtr<IMFMediaEventGenerator>& source) {
        std::lock_guard lock(mutex_);
        source_ = source;
        require(source_->BeginGetEvent(this, nullptr), "BeginGetEvent");
    }
    STDMETHOD(Invoke)(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEventGenerator> source;
        {
            std::lock_guard lock(mutex_);
            source = source_;
        }
        if (!source) return S_OK;

        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = source->EndGetEvent(result, event.GetAddressOf());
        {
            std::lock_guard lock(mutex_);
            if (!stopped_.load() && SUCCEEDED(hr) && event) {
                if (queue_.size() >= 64) {
                    overflow_.store(true);
                } else {
                    queue_.push_back(event);
                }
            } else if (!stopped_.load() && FAILED(hr)) {
                failure_.store(hr);
            }
        }
        SetEvent(signal_);
        if (SUCCEEDED(hr) && !stopped_.load()) {
            // Register again only while the transform is active.
            const HRESULT next = source->BeginGetEvent(this, nullptr);
            if (FAILED(next)) {
                failure_.store(next);
                SetEvent(signal_);
            }
        }
        return S_OK;
    }
    bool next(ComPtr<IMFMediaEvent>& output, DWORD milliseconds) {
        {
            std::lock_guard lock(mutex_);
            if (!queue_.empty()) {
                output = queue_.front();
                queue_.pop_front();
                return true;
            }
        }
        if (WaitForSingleObject(signal_, milliseconds) == WAIT_FAILED)
            throw std::runtime_error("WaitForSingleObject failed");
        if (overflow_.load()) throw std::runtime_error("Media event queue overflow");
        require(failure_.load(), "Asynchronous media event failure");
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return false;
        output = queue_.front();
        queue_.pop_front();
        return true;
    }
    void stop() {
        stopped_.store(true);
        {
            std::lock_guard lock(mutex_);
            source_.Reset();
            queue_.clear();
        }
        if (signal_) SetEvent(signal_);
    }
private:
    HANDLE signal_ = nullptr;
    std::mutex mutex_;
    ComPtr<IMFMediaEventGenerator> source_;
    std::deque<ComPtr<IMFMediaEvent>> queue_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> overflow_{false};
    std::atomic<HRESULT> failure_{S_OK};
};

class MfLifetime {
public:
    MfLifetime() { require(MFStartup(MF_VERSION), "MFStartup"); }
    ~MfLifetime() { MFShutdown(); }
};

class HardwareDevice {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    std::unique_ptr<gcp::media::GpuColorConverter> converter;

    explicit HardwareDevice(const Options& options) {
        require(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(),
            nullptr, context.GetAddressOf()), "D3D11CreateDevice(hardware)");
        UINT token = 0;
        require(MFCreateDXGIDeviceManager(&token, manager.GetAddressOf()),
                "MFCreateDXGIDeviceManager");
        require(manager->ResetDevice(device.Get(), token), "ResetDevice");
        if (options.use_bgra_gpu_conversion && !options.use_monitor_capture) {
            converter = std::make_unique<gcp::media::GpuColorConverter>(
                device.Get(), context.Get(),
                static_cast<UINT>(options.width),
                static_cast<UINT>(options.height),
                static_cast<UINT>(options.fps));
        }
    }

    ComPtr<IMFSample> frame(const Options& o, int index) {
        if (o.use_bgra_gpu_conversion) {
            // Synthetic SDR BGRA generated on CPU and uploaded to the same
            // D3D11 device. Conversion BGRA -> NV12 happens entirely on GPU.
            const auto pixels_count =
                static_cast<std::size_t>(o.width) * o.height;
            std::vector<std::uint8_t> pixels(pixels_count * 4);
            for (int y = 0; y < o.height; ++y) {
                for (int x = 0; x < o.width; ++x) {
                    const auto at =
                        (static_cast<std::size_t>(y) * o.width + x) * 4;
                    pixels[at] = static_cast<std::uint8_t>((x + index * 5) % 256);
                    pixels[at + 1] = static_cast<std::uint8_t>((y + index * 3) % 256);
                    pixels[at + 2] = 100;
                    pixels[at + 3] = 255;
                }
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(o.width);
            desc.Height = static_cast<UINT>(o.height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_SUBRESOURCE_DATA upload{};
            upload.pSysMem = pixels.data();
            upload.SysMemPitch = static_cast<UINT>(o.width * 4);
            ComPtr<ID3D11Texture2D> bgra;
            require(device->CreateTexture2D(
                &desc, &upload, bgra.GetAddressOf()),
                "Upload synthetic BGRA test texture");
            return converter->convert(
                bgra.Get(), (10'000'000LL * index) / o.fps,
                10'000'000LL / o.fps);
        }
        // Synthetic NV12 image, uploaded on creation to a D3D11 texture.
        // This is not GPU-resident BGRA->NV12 conversion yet.
        const auto ysize = static_cast<std::size_t>(o.width) * o.height;
        std::vector<std::uint8_t> pixels(ysize * 3 / 2, 128);
        for (int y = 0; y < o.height; ++y) {
            for (int x = 0; x < o.width; ++x) {
                pixels[static_cast<std::size_t>(y) * o.width + x] =
                    static_cast<std::uint8_t>(16 + ((x / 4 + y / 4 + 3 * index) % 220));
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(o.width);
        desc.Height = static_cast<UINT>(o.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = static_cast<UINT>(o.width);
        init.SysMemSlicePitch = static_cast<UINT>(pixels.size());

        ComPtr<ID3D11Texture2D> texture;
        require(device->CreateTexture2D(&desc, &init, texture.GetAddressOf()),
                "Create initialized NV12 GPU texture");
        ComPtr<IMFMediaBuffer> buffer;
        require(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                          texture.Get(), 0, FALSE,
                                          buffer.GetAddressOf()),
                "Create DXGI MF buffer");
        ComPtr<IMFSample> sample;
        require(MFCreateSample(sample.GetAddressOf()), "Create IMF sample");
        require(sample->AddBuffer(buffer.Get()), "Add GPU-backed buffer");
        require(sample->SetSampleTime((10'000'000LL * index) / o.fps),
                "Set sample time");
        require(sample->SetSampleDuration(10'000'000LL / o.fps),
                "Set sample duration");
        return sample;
    }
};

struct Activations {
    IMFActivate** entries = nullptr;
    UINT32 count = 0;
    ~Activations() {
        for (UINT32 i = 0; i < count; ++i) {
            if (entries[i]) entries[i]->Release();
        }
        CoTaskMemFree(entries);
    }
};

ComPtr<IMFMediaType> video_type(const GUID& subtype,
                               const Options& options, bool compressed) {
    ComPtr<IMFMediaType> media;
    require(MFCreateMediaType(media.GetAddressOf()), "Create media type");
    require(media->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Major type");
    require(media->SetGUID(MF_MT_SUBTYPE, subtype), "Subtype");
    require(media->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
            "Progressive video");
    require(MFSetAttributeSize(media.Get(), MF_MT_FRAME_SIZE,
                               static_cast<UINT32>(options.width),
                               static_cast<UINT32>(options.height)), "Frame size");
    require(MFSetAttributeRatio(media.Get(), MF_MT_FRAME_RATE,
                                static_cast<UINT32>(options.fps), 1), "Frame rate");
    require(MFSetAttributeRatio(media.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
            "Pixel aspect ratio");
    if (compressed) {
        require(media->SetUINT32(
            MF_MT_AVG_BITRATE, static_cast<UINT32>(options.bitrate)), "Bitrate");
    }
    return media;
}

struct OutputRecord {
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t submitted_inputs = 0;
    std::uint64_t monitor_idle_waits = 0;
    bool time_limit_reached = false;
    std::unique_ptr<gcp::transport::FrameArrivalMetrics> timing;
};

// Only process output after an actual asynchronous HaveOutput event.
// The caller must provide an output buffer if the MFT does not provide one.
void receive_output(IMFTransform* encoder, DWORD output_id,
                    const MFT_OUTPUT_STREAM_INFO& info, OutputRecord& totals,
                    const gcp::media::HardwareH264Callback* on_encoded) {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = output_id;
    ComPtr<IMFSample> sample;
    const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    if (!provides) {
        ComPtr<IMFMediaBuffer> buffer;
        require(MFCreateMemoryBuffer(std::max<DWORD>(4096u, info.cbSize),
                                     buffer.GetAddressOf()), "Create output buffer");
        require(MFCreateSample(sample.GetAddressOf()), "Create output sample");
        require(sample->AddBuffer(buffer.Get()), "Attach output buffer");
        output.pSample = sample.Get();
    }

    DWORD status = 0;
    const HRESULT result = encoder->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (provides) sample.Attach(output.pSample);
    if (FAILED(result)) require(result, "Hardware MFT ProcessOutput");
    if (!sample) throw std::runtime_error("MFT returned no encoded output sample");

    ComPtr<IMFMediaBuffer> bytes;
    require(sample->ConvertToContiguousBuffer(bytes.GetAddressOf()),
            "Contiguous encoded buffer");
    DWORD used = 0;
    require(bytes->GetCurrentLength(&used), "Encoded buffer length");
    if (used == 0 || used > 4 * 1024 * 1024)
        throw std::runtime_error("Empty or oversized H.264 output sample");

    UINT32 keyframe = 0;
    const bool is_keyframe =
        SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &keyframe)) &&
        keyframe != 0;

    if (on_encoded && *on_encoded) {
        LONGLONG timestamp = 0;
        require(sample->GetSampleTime(&timestamp),
                "H.264 encoded sample timestamp");
        if (timestamp < 0) throw std::runtime_error("Negative H.264 timestamp");
        BYTE* raw = nullptr;
        DWORD maximum = 0;
        DWORD current = 0;
        require(bytes->Lock(&raw, &maximum, &current),
                "Lock H.264 encoded payload");
        gcp::media::HardwareH264AccessUnit packet;
        try {
            if (current != used)
                throw std::runtime_error("H.264 payload size changed");
            packet.bytes.assign(raw, raw + current);
        } catch (...) {
            bytes->Unlock();
            throw;
        }
        require(bytes->Unlock(), "Unlock encoded H.264 payload");
        packet.timestamp_us = timestamp / 10;
        packet.keyframe = is_keyframe;
        if (is_keyframe) {
            // Windows encoders may attach Annex-B SPS/PPS to their H.264
            // output media type rather than include them in every IDR packet.
            ComPtr<IMFMediaType> current_type;
            if (SUCCEEDED(encoder->GetOutputCurrentType(
                    output_id, current_type.GetAddressOf()))) {
                UINT32 size = 0;
                if (SUCCEEDED(current_type->GetBlobSize(
                        MF_MT_MPEG_SEQUENCE_HEADER, &size))) {
                    if (size > 64u * 1024u)
                        throw std::runtime_error(
                            "Invalid MF H.264 parameter-set header size");
                    if (size != 0) {
                        packet.parameter_sets.resize(size);
                        UINT32 actual = 0;
                        require(current_type->GetBlob(
                            MF_MT_MPEG_SEQUENCE_HEADER,
                            packet.parameter_sets.data(), size, &actual),
                            "Read H.264 parameter sets");
                        packet.parameter_sets.resize(actual);
                    }
                }
            }
        }
        if (!(*on_encoded)(packet))
            throw std::runtime_error("Hardware H.264 transport rejected encoded packet");
    }
    if (is_keyframe) ++totals.keyframes;
    totals.bytes += used;
    ++totals.samples;
    if (totals.timing) totals.timing->record(Clock::now());
}

bool encode_candidate(IMFActivate* activation, HardwareDevice& gpu,
                      const Options& options, OutputRecord& totals,
                      const gcp::media::HardwareH264Callback* on_encoded,
                      const gcp::media::HardwareH264FeedbackCallback* feedback,
                      gcp::media::HardwareH264FeedbackStats* feedback_stats,
                      const gcp::media::ScreenH264Session* controlled) {
    ComPtr<IMFTransform> encoder;
    if (FAILED(activation->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()))))
        return false;

    struct Cleanup {
        IMFActivate* active;
        ~Cleanup() { active->ShutdownObject(); }
    } cleanup{activation};

    ComPtr<IMFAttributes> attributes;
    if (FAILED(encoder->GetAttributes(attributes.GetAddressOf()))) return false;
    UINT32 d3d11_aware = 0;
    if (FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware))
        || !d3d11_aware) return false;

    UINT32 asynchronous = 0;
    if (FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous))
        || !asynchronous) {
        // No unsafe synchronous fallback in this asynchronous prototype.
        return false;
    }
    require(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE),
            "Unlock async transform");

    DWORD input_id = 0, output_id = 0, inputs = 0, outputs = 0;
    require(encoder->GetStreamCount(&inputs, &outputs), "Stream counts");
    if (inputs != 1 || outputs != 1) return false;
    const HRESULT stream_ids = encoder->GetStreamIDs(1, &input_id, 1, &output_id);
    if (stream_ids != E_NOTIMPL) require(stream_ids, "Stream IDs");

    if (FAILED(encoder->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(gpu.manager.Get())))) return false;

    auto compressed = video_type(MFVideoFormat_H264, options, true);
    auto uncompressed = video_type(MFVideoFormat_NV12, options, false);
    if (FAILED(encoder->SetOutputType(output_id, compressed.Get(), 0)) ||
        FAILED(encoder->SetInputType(input_id, uncompressed.Get(), 0))) return false;

    MFT_OUTPUT_STREAM_INFO info{};
    require(encoder->GetOutputStreamInfo(output_id, &info), "Output stream info");

    // ICodecAPI is available on some, not all, hardware-class MFTs.
    // Unsupported controls are recorded rather than falsely reported as
    // successfully applied or failing an otherwise valid diagnostic.
    ComPtr<ICodecAPI> codec;
    (void)encoder.As(&codec);
    ComPtr<IMFMediaEventGenerator> events;
    require(encoder.As(&events), "Async IMFMediaEventGenerator");
    auto pump = Microsoft::WRL::Make<EventPump>();
    if (!pump) throw std::runtime_error("Unable to allocate event pump");
    pump->start(events);

    // Construct only after the MFT has accepted this exact GPU device and
    // media format. This is strictly local and requires explicit CLI consent.
    std::unique_ptr<gcp::media::LiveMonitorCapture> monitor_capture;
    std::uint64_t monitor_sequence = 0;
    try {
    if (options.use_monitor_capture) {
        if (controlled) {
            if (!controlled->keep_running()) return true;
            monitor_capture = std::make_unique<gcp::media::LiveMonitorCapture>(
                gpu.device.Get(), gpu.context.Get(), controlled->item,
                options.width, options.height, options.fps);
            if (!controlled->keep_running()) return true;
            if (controlled->capture_started) controlled->capture_started();
        } else {
            monitor_capture = std::make_unique<gcp::media::LiveMonitorCapture>(
                gpu.device.Get(), gpu.context.Get(), options.monitor_index,
                options.width, options.height, options.fps);
        }
        monitor_capture->start();
        std::cerr << "CAPTURING MONITOR " << options.monitor_index
                  << (on_encoded ? " into hardware encoder with transport callback\n"
                                 : " into local hardware encoder (not transmitted)\n");
    }

        require(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                "Begin streaming");
        require(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                "Start of stream");
        const auto session_started = Clock::now();
        const auto deadline = session_started +
                              std::chrono::seconds(options.timeout_seconds);
        const auto input_cutoff = options.duration_seconds > 0
            ? session_started + std::chrono::seconds(options.duration_seconds)
            : deadline;
        totals.timing = std::make_unique<gcp::transport::FrameArrivalMetrics>(
            session_started);
        std::uint64_t submitted = 0;
        bool draining = false, complete = false;
        auto start_drain = [&]() {
            if (draining) return;
            require(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "End of stream");
            require(encoder->ProcessMessage(
                MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain hardware MFT");
            draining = true;
        };
        const auto active = [&] { return !controlled || controlled->keep_running(); };
        while ((controlled || Clock::now() < deadline) && !complete && active()) {
            if (monitor_capture && !monitor_capture->healthy())
                throw std::runtime_error("Selected capture source closed or changed");
            if (options.duration_seconds > 0 &&
                Clock::now() >= input_cutoff && !draining) {
                totals.time_limit_reached = true;
                start_drain();
            }
            ComPtr<IMFMediaEvent> event;
            if (!pump->next(event, 200)) continue;
            MediaEventType kind = MEUnknown;
            require(event->GetType(&kind), "Media event type");

            if (kind == METransformNeedInput && !draining &&
                (controlled || submitted < static_cast<std::uint64_t>(options.frames))) {
                ComPtr<IMFSample> sample;
                if (monitor_capture) {
                    // Static screens may legitimately produce no new WGC
                    // frames. Keep the input request outstanding until a
                    // fresh frame, the explicit duration, or a real failure.
                    while (active() && (controlled || (Clock::now() < input_cutoff &&
                           Clock::now() < deadline)) && !sample) {
                        if (monitor_capture->take_next(
                                monitor_sequence, sample, 250)) break;
                        if (!monitor_capture->healthy()) {
                            throw std::runtime_error(
                                "Monitor capture stopped (resize or GPU failure)");
                        }
                        ++totals.monitor_idle_waits;
                    }
                    if (!active()) break;
                    if (!sample) {
                        if (options.duration_seconds > 0) {
                            totals.time_limit_reached = true;
                            start_drain();
                            continue;
                        }
                        throw std::runtime_error(
                            "Monitor did not produce a fresh frame before timeout");
                    }
                } else {
                    sample = gpu.frame(options, static_cast<int>(submitted));
                }
                if (feedback && *feedback) {
                    const auto request = (*feedback)();
                    auto set_codec_u32 = [&](const GUID& property, ULONG value) {
                        if (!codec) return false;
                        VARIANT requested{};
                        requested.vt = VT_UI4;
                        requested.ulVal = value;
                        return SUCCEEDED(codec->SetValue(&property, &requested));
                    };
                    if (request.force_keyframe) {
                        if (feedback_stats) ++feedback_stats->keyframe_requests;
                        const bool applied = set_codec_u32(
                            CODECAPI_AVEncVideoForceKeyFrame, 1);
                        if (feedback_stats) {
                            if (applied) ++feedback_stats->keyframe_controls_accepted;
                            else ++feedback_stats->keyframe_controls_unsupported;
                        }
                    }
                    if (request.target_bitrate_bps != 0) {
                        if (feedback_stats) ++feedback_stats->bitrate_requests;
                        const bool in_range =
                            request.target_bitrate_bps >= 100'000 &&
                            request.target_bitrate_bps <= 16'000'000;
                        const bool applied = in_range && set_codec_u32(
                            CODECAPI_AVEncCommonMeanBitRate,
                            request.target_bitrate_bps);
                        if (feedback_stats) {
                            if (applied) ++feedback_stats->bitrate_controls_accepted;
                            else ++feedback_stats->bitrate_controls_unsupported;
                        }
                    }
                }
                if (!active()) break;
                require(encoder->ProcessInput(input_id, sample.Get(), 0),
                        "GPU NV12 ProcessInput");
                ++submitted;
                ++totals.submitted_inputs;
                if (!controlled && submitted == static_cast<std::uint64_t>(options.frames)) start_drain();
            } else if (kind == METransformHaveOutput && active()) {
                receive_output(encoder.Get(), output_id, info, totals, on_encoded);
            } else if (kind == METransformDrainComplete && draining) {
                complete = true;
            } else if (kind == MEError) {
                throw std::runtime_error("Hardware MFT raised a media error event");
            }
        }
        pump->stop();
        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (monitor_capture) {
            std::cout << "{\"monitor_frames_received\":"
                      << monitor_capture->received()
                      << ",\"gpu_frames_converted\":"
                      << monitor_capture->converted()
                      << ",\"frames_dropped\":"
                      << monitor_capture->dropped()
                      << ",\"capture_failures\":"
                      << monitor_capture->failures()
                      << ",\"cpu_pixel_readbacks\":0}\n";
        }
        if (controlled) return !active();
        if (!complete || totals.samples == 0) {
            std::cerr << "Hardware MFT did not complete within test deadline\n";
            return false;
        }
        return true;
    } catch (...) {
        pump->stop();
        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        throw;
    }
}

int run(const Options& options,
        const gcp::media::HardwareH264Callback* on_encoded = nullptr,
        const gcp::media::HardwareH264FeedbackCallback* feedback = nullptr,
        gcp::media::HardwareH264FeedbackStats* feedback_stats = nullptr,
        const gcp::media::ScreenH264Session* controlled = nullptr) {
    if (options.use_monitor_capture)
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    MfLifetime media;
    HardwareDevice gpu(options);
    MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, MFVideoFormat_H264};
    Activations list;
    require(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                      nullptr, &output_type, &list.entries, &list.count),
            "Hardware H264 MFT enumeration");

    const auto start = Clock::now();
    const auto process_before = gcp::probe::take_snapshot();
    std::cout << "Hardware-class H.264 candidates: " << list.count << "\n";
    for (UINT32 i = 0; i < list.count; ++i) {
        OutputRecord output;
        try {
            if (encode_candidate(list.entries[i], gpu, options, output,
                                 on_encoded, feedback, feedback_stats, controlled)) {
                const auto end = Clock::now();
                const auto elapsed =
                    std::chrono::duration<double>(end - start).count();
                const auto process_after = gcp::probe::take_snapshot();
                const auto latency = output.timing
                    ? output.timing->report(end)
                    : gcp::transport::FrameArrivalMetrics::Report{};
                std::cout << "{\"probe\":\"hardware_async_h264\""
                          << ",\"encoded_samples\":" << output.samples
                          << ",\"submitted_input_samples\":" << output.submitted_inputs
                          << ",\"monitor_idle_waits\":" << output.monitor_idle_waits
                          << ",\"time_limit_reached\":"
                          << (output.time_limit_reached ? "true" : "false")
                          << ",\"encoded_bytes\":" << output.bytes
                          << ",\"measured_output_bits_per_second\":"
                          << (elapsed > 0
                              ? static_cast<double>(output.bytes) * 8.0 / elapsed : 0.0)
                          << ",\"time_to_first_encoded_output_ms\":"
                          << latency.time_to_first_ms
                          << ",\"encoded_output_arrival_p50_ms\":"
                          << latency.interval_p50_ms
                          << ",\"encoded_output_arrival_p95_ms\":"
                          << latency.interval_p95_ms
                          << ",\"encoded_output_timing_samples\":"
                          << latency.interval_samples
                          << ",\"cpu_percent_total_machine\":"
                          << gcp::probe::total_machine_cpu_percent(
                                 process_before, process_after, elapsed,
                                 gcp::probe::logical_processor_count())
                          << ",\"private_bytes_start\":"
                          << process_before.private_bytes
                          << ",\"private_bytes_end\":"
                          << process_after.private_bytes
                          << ",\"keyframes\":" << output.keyframes
                          << ",\"elapsed_seconds\":" << elapsed
                          << ",\"hardware_class_mft\":true"
                          << ",\"real_monitor_capture\":"
                          << (options.use_monitor_capture ? "true" : "false")
                          << ",\"gpu_bgra_to_nv12_requested\":"
                          << (options.use_bgra_gpu_conversion ? "true" : "false")
                          << ",\"livekit_connected\":"
                          << (on_encoded ? "true" : "false")
                          << ",\"gpu_engine_utilization_verified\":false}\n";
                return 0;
            }
        } catch (const std::exception& e) {
            std::cerr << "Candidate " << i << " failed: " << e.what() << "\n";
            // Once a transport callback is active, never try a different
            // encoder after sending partial packets to the same LiveKit track.
            if (on_encoded) throw;
        }
    }
    std::cerr << "No asynchronous D3D11-aware hardware-class H.264 MFT "
                 "produced output on this GPU/configuration\n";
    return 2;
}
} // namespace

namespace gcp::media {
int encode_monitor_h264(
    const HardwareH264Profile& profile,
    const HardwareH264Callback& callback,
    const HardwareH264FeedbackCallback& feedback,
    HardwareH264FeedbackStats* feedback_stats) {
    if (!callback)
        throw std::invalid_argument("H.264 output callback is required");
    Options options;
    options.width = profile.width;
    options.height = profile.height;
    options.fps = profile.fps;
    options.frames = profile.frames;
    options.bitrate = profile.bitrate_bps;
    options.timeout_seconds = profile.timeout_seconds;
    options.duration_seconds = profile.duration_seconds;
    options.monitor_index = profile.monitor_index;
    options.confirmed_monitor_capture = profile.monitor_capture_confirmed;
    options.use_bgra_gpu_conversion = true;
    options.use_monitor_capture = true;
    if (!valid(options))
        throw std::invalid_argument("Monitor H.264 source needs explicit consent and valid dimensions");
    return run(options, &callback,
               feedback ? &feedback : nullptr, feedback_stats);
}
int encode_screen_h264(
    const HardwareH264Profile& profile, const ScreenH264Session& session,
    const HardwareH264Callback& callback,
    const HardwareH264FeedbackCallback& feedback) {
    if (!callback || !session.item || !session.keep_running)
        throw std::invalid_argument("Controlled screen source is required");
    Options options;
    options.width = profile.width;
    options.height = profile.height;
    options.fps = profile.fps;
    options.bitrate = profile.bitrate_bps;
    options.use_monitor_capture = true;
    options.use_bgra_gpu_conversion = true;
    // This internal path accepts only the retained picker item and a live
    // consent/lease watchdog, never the diagnostic's monitor index.
    options.monitor_index = 0;
    options.confirmed_monitor_capture = true;
    if (!valid(options)) throw std::invalid_argument("Invalid selected source profile");
    return run(options, &callback, feedback ? &feedback : nullptr, nullptr, &session);
}
} // namespace gcp::media

#ifndef GCP_HARDWARE_H264_NO_MAIN
int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            usage();
            return 0;
        }
        if (std::string_view(argv[1]) == "--self-test") {
            if (argc != 2) return 1;
            char program[] = "probe";
            char run_mode[] = "--run";
            char width[] = "--width";
            char odd[] = "331";
            char* invalid[] = {program, run_mode, width, odd};
            try { (void)parse(4, invalid); return 1; }
            catch (const std::invalid_argument&) {}
            char good[] = "640";
            char* valid_args[] = {program, run_mode, width, good};
            const auto o = parse(4, valid_args);
            if (!valid(o) || o.width != 640) return 1;
            char source_flag[] = "--source";
            char bgra[] = "bgra";
            char* with_source[] = {program, run_mode, source_flag, bgra};
            const auto selected = parse(4, with_source);
            if (!selected.use_bgra_gpu_conversion) return 1;
            char monitor_value[] = "monitor";
            char* unconfirmed[] = {program, run_mode, source_flag, monitor_value};
            try { (void)parse(4, unconfirmed); return 1; }
            catch (const std::invalid_argument&) {}
            char monitor_flag[] = "--monitor";
            char monitor_index[] = "0";
            char confirm_flag[] = "--confirm-monitor-share";
            char yes[] = "yes";
            char* confirmed[] = {program, run_mode, source_flag, monitor_value,
                                 monitor_flag, monitor_index, confirm_flag, yes};
            const auto shared = parse(8, confirmed);
            if (!shared.use_monitor_capture || shared.monitor_index != 0 ||
                !shared.confirmed_monitor_capture) return 1;
            char duration_flag[] = "--duration";
            char duration_time[] = "120";
            char* stable_args[] = {program, run_mode, source_flag, monitor_value,
                                   monitor_flag, monitor_index, confirm_flag, yes,
                                   duration_flag, duration_time};
            const auto stable = parse(10, stable_args);
            if (stable.frames != 3600 || stable.duration_seconds != 120 ||
                stable.timeout_seconds != 135) return 1;
            char too_long[] = "301";
            char* bad_duration[] = {program, run_mode, source_flag, monitor_value,
                                    monitor_flag, monitor_index, confirm_flag, yes,
                                    duration_flag, too_long};
            try { (void)parse(10, bad_duration); return 1; }
            catch (const std::invalid_argument&) {}
            char huge[] = "2147483647";
            char fps_flag[] = "--fps";
            char* overflow_profile[] = {program, run_mode, fps_flag, huge,
                                       duration_flag, duration_time};
            try { (void)parse(6, overflow_profile); return 1; }
            catch (const std::invalid_argument&) {}
            char invalid_source[] = "gpu";
            char* bad_source[] = {program, run_mode, source_flag, invalid_source};
            try { (void)parse(4, bad_source); return 1; }
            catch (const std::invalid_argument&) {}
            std::cout << "Hardware H.264 CLI self-test passed\n";
            return 0;
        }
        if (std::string_view(argv[1]) != "--run")
            throw std::invalid_argument("Specify --run or --help");
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "Hardware H.264 diagnostic failed: " << e.what() << "\n";
        return 1;
    }
}

#endif // GCP_HARDWARE_H264_NO_MAIN
