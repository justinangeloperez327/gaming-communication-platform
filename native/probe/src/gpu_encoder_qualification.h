#pragma once

namespace gcp::probe {

struct GpuH264Profile {
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate_bps = 4'000'000;
};

bool valid_gpu_h264_profile(const GpuH264Profile& profile);

// Hardware-only encoder negotiation: D3D11 device and Media Foundation GPU
// surface -> registered hardware MFT. No actual H.264 access units produced.
// Returns 0 only when at least one device-aware candidate negotiates formats,
// 2 if no candidate qualifies, 1 for a fatal system/API failure.
int qualify_gpu_h264(const GpuH264Profile& profile);

} // namespace gcp::probe
