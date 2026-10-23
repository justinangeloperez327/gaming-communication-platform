#pragma once
#include "hardware_h264_bridge.h"
#include <winrt/Windows.Graphics.Capture.h>

namespace gcp::media {
// The exact picker result stays alive in the child. No source re-enumeration.
// keep_running is checked during static-screen waits and before input/output.
// Stop flushes rather than draining old frames into the network.
struct ScreenH264Session {
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    std::function<bool()> keep_running;
    std::function<void()> capture_started;
    int source_width = 0;
    int source_height = 0;
};
int encode_screen_h264(const HardwareH264Profile& profile,
                       const ScreenH264Session& session,
                       const HardwareH264Callback& callback,
                       const HardwareH264FeedbackCallback& feedback = {});
} // namespace gcp::media
