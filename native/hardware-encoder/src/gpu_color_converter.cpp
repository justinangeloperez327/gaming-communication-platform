#include "gpu_color_converter.h"

#include <mfapi.h>
#include <mferror.h>

#include <stdexcept>
#include <string>

namespace gcp::media {
namespace {
void require(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(operation) + " (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}
}

GpuColorConverter::GpuColorConverter(
    ID3D11Device* device, ID3D11DeviceContext* context,
    UINT width, UINT height, UINT fps, UINT input_width, UINT input_height)
    : width_(width), height_(height),
      input_width_(input_width ? input_width : width),
      input_height_(input_height ? input_height : height), device_(device) {
    if (!device || !context || width < 160 || width > 1920 ||
        height < 90 || height > 1080 || (width % 2) || (height % 2) ||
        fps < 1 || fps > 60 || input_width_ < 160 || input_width_ > 1920 ||
        input_height_ < 90 || input_height_ > 1080) {
        throw std::invalid_argument("Invalid hardware BGRA to NV12 conversion configuration");
    }

    require(device->QueryInterface(IID_PPV_ARGS(video_device_.GetAddressOf())),
            "Query D3D11 video device");
    require(context->QueryInterface(IID_PPV_ARGS(video_context_.GetAddressOf())),
            "Query D3D11 video context");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contents{};
    contents.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contents.InputWidth = input_width_;
    contents.InputHeight = input_height_;
    contents.OutputWidth = width;
    contents.OutputHeight = height;
    contents.InputFrameRate.Numerator = fps;
    contents.InputFrameRate.Denominator = 1;
    contents.OutputFrameRate = contents.InputFrameRate;
    contents.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    require(video_device_->CreateVideoProcessorEnumerator(
                &contents, enumerator_.GetAddressOf()),
            "Create GPU video-processor enumerator");

    UINT input_support = 0;
    UINT output_support = 0;
    require(enumerator_->CheckVideoProcessorFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM, &input_support),
            "Check BGRA video-processor input");
    require(enumerator_->CheckVideoProcessorFormat(
                DXGI_FORMAT_NV12, &output_support),
            "Check NV12 video-processor output");
    if (!(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        throw std::runtime_error(
            "GPU video processor does not support BGRA input and NV12 output");
    }

    require(video_device_->CreateVideoProcessor(
                enumerator_.Get(), 0, processor_.GetAddressOf()),
            "Create GPU video processor");

    RECT region{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    RECT source_region{0, 0, static_cast<LONG>(input_width_),
                       static_cast<LONG>(input_height_)};
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamSourceRect(
        processor_.Get(), 0, TRUE, &source_region);
    video_context_->VideoProcessorSetStreamDestRect(
        processor_.Get(), 0, TRUE, &region);
    video_context_->VideoProcessorSetOutputTargetRect(
        processor_.Get(), TRUE, &region);
    // BGRA8 capture is SDR-only. Preserve the full source rectangle, including
    // odd window dimensions, by GPU conversion to the nearest even NV12 size.
    // HDR tone mapping is not implemented.
}

Microsoft::WRL::ComPtr<IMFSample> GpuColorConverter::convert(
    ID3D11Texture2D* bgra, LONGLONG timestamp_100ns,
    LONGLONG duration_100ns) {
    if (!bgra || timestamp_100ns < 0 || duration_100ns <= 0) {
        throw std::invalid_argument("Invalid BGRA source or sample timing");
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    bgra->GetDesc(&source_desc);
    if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        source_desc.Width != input_width_ || source_desc.Height != input_height_ ||
        source_desc.ArraySize != 1 || source_desc.SampleDesc.Count != 1 ||
        source_desc.Usage != D3D11_USAGE_DEFAULT) {
        throw std::invalid_argument("Incompatible BGRA source texture");
    }

    Microsoft::WRL::ComPtr<ID3D11Device> source_device;
    bgra->GetDevice(source_device.GetAddressOf());
    if (source_device.Get() != device_.Get()) {
        throw std::invalid_argument("Source and video processor must use the same GPU device");
    }

    // Distinct output texture per in-flight frame: never reuse NV12 memory
    // before the asynchronous MFT releases its IMF sample references.
    D3D11_TEXTURE2D_DESC target{};
    target.Width = width_;
    target.Height = height_;
    target.MipLevels = 1;
    target.ArraySize = 1;
    target.Format = DXGI_FORMAT_NV12;
    target.SampleDesc.Count = 1;
    target.Usage = D3D11_USAGE_DEFAULT;
    target.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12;
    require(device_->CreateTexture2D(
                &target, nullptr, nv12.GetAddressOf()),
            "Allocate NV12 video-processor output surface");

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    require(video_device_->CreateVideoProcessorInputView(
                bgra, enumerator_.Get(), &input_desc,
                input_view.GetAddressOf()),
            "Create BGRA video input view");

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    require(video_device_->CreateVideoProcessorOutputView(
                nv12.Get(), enumerator_.Get(), &output_desc,
                output_view.GetAddressOf()),
            "Create NV12 video output view");

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    require(video_context_->VideoProcessorBlt(
                processor_.Get(), output_view.Get(), 0, 1, &stream),
            "GPU BGRA to NV12 VideoProcessorBlt");

    Microsoft::WRL::ComPtr<IMFMediaBuffer> media_buffer;
    require(MFCreateDXGISurfaceBuffer(
                __uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE,
                media_buffer.GetAddressOf()),
            "Wrap converted NV12 GPU texture");
    Microsoft::WRL::ComPtr<IMFSample> sample;
    require(MFCreateSample(sample.GetAddressOf()), "Create converted video sample");
    require(sample->AddBuffer(media_buffer.Get()), "Attach converted NV12 GPU sample");
    require(sample->SetSampleTime(timestamp_100ns), "Converted sample timestamp");
    require(sample->SetSampleDuration(duration_100ns), "Converted sample duration");
    return sample;
}

} // namespace gcp::media
