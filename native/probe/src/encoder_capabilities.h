#pragma once

namespace gcp::probe {

// Diagnostics only: enumeration is not proof that an encoder can be used
// by LiveKit, accepts requested resolutions, or provides hardware encoding.
void list_gpu_adapters();
void list_video_encoder_candidates();
int probe_hardware_h264_activation();

} // namespace gcp::probe
