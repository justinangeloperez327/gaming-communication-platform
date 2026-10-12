#include "h264_encode_benchmark.h"
#include "telemetry.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gcp::probe {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(
            std::string(what) + " (HRESULT " + std::to_string(
                static_cast<unsigned long>(hr)) + ")");
    }
}

struct MediaFoundationScope {
    MediaFoundationScope() { check(MFStartup(MF_VERSION), "MFStartup"); }
    ~MediaFoundationScope() { MFShutdown(); }
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
};

// Output contains a synthetic pattern, never screen or microphone content.
// A unique temporary .mp4 is deleted even when an exception is raised.
class TemporaryOutput {
public:
    TemporaryOutput() {
        wchar_t directory[MAX_PATH + 1]{};
        const auto length = GetTempPathW(MAX_PATH, directory);
        if (length == 0 || length > MAX_PATH)
            throw std::runtime_error("Unable to locate temporary directory");
        wchar_t candidate[MAX_PATH + 1]{};
        if (!GetTempFileNameW(directory, L"gcp", 0, candidate))
            throw std::runtime_error("Unable to reserve temporary output");
        path_ = candidate;
        // The reserved file is ours and only synthetic frames are written.
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        if (ec) throw std::runtime_error("Unable to prepare output path");
        path_ += L".mp4";
        if (std::filesystem::exists(path_))
            throw std::runtime_error("Temporary output collision");
    }
    ~TemporaryOutput() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const wchar_t* c_str() const { return path_.c_str(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void fill_nv12(std::uint8_t* pixels, int width, int height, int index) {
    const auto y_bytes = static_cast<std::size_t>(width) *
                         static_cast<std::size_t>(height);
    for (int row = 0; row < height; ++row) {
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(row) * width + x] =
                static_cast<std::uint8_t>(
                    16 + ((x / 4 + row / 4 + index * 3) % 220));
        }
    }
    // Neutral chroma creates a reproducible grayscale scene.
    for (auto at = y_bytes; at < y_bytes + y_bytes / 2; at += 2) {
        pixels[at] = 128;
        pixels[at + 1] = 128;
    }
}

ComPtr<IMFMediaType> make_video_type(const GUID& subtype, int width,
                                     int height, int fps) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(type.GetAddressOf()), "Create media type");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Major video type");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Video subtype");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
          "Progressive mode");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                             static_cast<UINT32>(width),
                             static_cast<UINT32>(height)), "Frame size");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                              static_cast<UINT32>(fps), 1), "Frame rate");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
          "Pixel aspect ratio");
    return type;
}

void write_frame(IMFSinkWriter* writer, DWORD stream_index,
                 int width, int height, int fps, int index) {
    const auto bytes = static_cast<DWORD>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2);
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(bytes, buffer.GetAddressOf()), "Allocate NV12 buffer");

    BYTE* pixels = nullptr;
    DWORD maximum = 0;
    check(buffer->Lock(&pixels, &maximum, nullptr), "Lock NV12 buffer");
    try {
        fill_nv12(pixels, width, height, index);
    } catch (...) {
        buffer->Unlock();
        throw;
    }
    check(buffer->Unlock(), "Unlock NV12 buffer");
    check(buffer->SetCurrentLength(bytes), "Set NV12 buffer length");

    ComPtr<IMFSample> sample;
    check(MFCreateSample(sample.GetAddressOf()), "Create video sample");
    check(sample->AddBuffer(buffer.Get()), "Add video buffer");
    const auto timestamp = (10'000'000LL * index) / fps;
    const auto duration = 10'000'000LL / fps;
    check(sample->SetSampleTime(timestamp), "Sample timestamp");
    check(sample->SetSampleDuration(duration), "Sample duration");
    check(writer->WriteSample(stream_index, sample.Get()), "Encode H.264 frame");
}
} // namespace

int benchmark_h264_encoding(int width, int height, int fps, int frames,
                            int bitrate_bps) {
    if (width < 160 || width > 1920 || (width % 2) ||
        height < 90 || height > 1080 || (height % 2) ||
        fps < 1 || fps > 60 || frames < 1 || frames > 180 ||
        bitrate_bps < 100'000 || bitrate_bps > 16'000'000)
        throw std::invalid_argument("Unsupported H.264 benchmark parameters");

    MediaFoundationScope media;
    TemporaryOutput output;

    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(attributes.GetAddressOf(), 1),
          "Create sink writer attributes");
    check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
          "Request hardware transforms");

    ComPtr<IMFSinkWriter> writer;
    check(MFCreateSinkWriterFromURL(output.c_str(), nullptr, attributes.Get(),
                                    writer.GetAddressOf()),
          "Create H.264 MP4 sink writer");

    auto compressed = make_video_type(MFVideoFormat_H264, width, height, fps);
    check(compressed->SetUINT32(MF_MT_AVG_BITRATE,
                                static_cast<UINT32>(bitrate_bps)),
          "Set H.264 bitrate");
    DWORD stream = 0;
    check(writer->AddStream(compressed.Get(), &stream),
          "Configure compressed H.264 output");

    const auto uncompressed = make_video_type(MFVideoFormat_NV12, width, height, fps);
    check(writer->SetInputMediaType(stream, uncompressed.Get(), nullptr),
          "Configure NV12 input");
    check(writer->BeginWriting(), "Begin H.264 encoding");

    const auto start = Clock::now();
    const auto before = take_snapshot();
    for (int index = 0; index < frames; ++index) {
        write_frame(writer.Get(), stream, width, height, fps, index);
    }
    check(writer->Finalize(), "Finalize H.264 MP4");
    const auto after = take_snapshot();
    const auto seconds = std::chrono::duration<double>(
        Clock::now() - start).count();
    writer.Reset();

    const auto size = std::filesystem::file_size(output.path());
    std::cout << std::fixed << std::setprecision(3)
              << "{\"probe\":\"mf_h264_encode\""
              << ",\"source\":\"synthetic_nv12\""
              << ",\"frames\":" << frames
              << ",\"width\":" << width
              << ",\"height\":" << height
              << ",\"fps_configured\":" << fps
              << ",\"frames_per_second_processed\":"
              << (seconds > 0 ? frames / seconds : 0)
              << ",\"bitrate_bps_requested\":" << bitrate_bps
              << ",\"encoded_mp4_bytes\":" << size
              << ",\"elapsed_seconds\":" << seconds
              << ",\"cpu_percent_total_machine\":"
              << total_machine_cpu_percent(
                    before, after, seconds, logical_processor_count())
              << ",\"private_bytes_start\":" << before.private_bytes
              << ",\"private_bytes_end\":" << after.private_bytes
              << ",\"hardware_transforms_requested\":true"
              << ",\"hardware_encoder_confirmed\":false"
              << "}\n";
    if (size < 256u)
        throw std::runtime_error("Encoded file was unexpectedly small");
    return 0;
}
} // namespace gcp::probe
