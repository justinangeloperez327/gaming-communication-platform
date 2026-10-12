#pragma once

namespace gcp::probe {

// Encodes synthetic NV12 frames into a temporary MP4 with a real
// Media Foundation H.264 Sink Writer. Requests hardware transforms but
// intentionally does not claim that a GPU encoder was selected.
int benchmark_h264_encoding(int width, int height, int fps, int frames,
                            int bitrate_bps);

} // namespace gcp::probe
