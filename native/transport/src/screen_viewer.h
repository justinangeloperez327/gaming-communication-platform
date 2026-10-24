#pragma once
#include "view_control.h"
#include "livekit/video_frame.h"
#include "livekit/video_source.h"
#include <functional>

namespace gcp::transport {
using ViewEvent = std::function<void(const std::string&)>;
using ViewPixels = std::function<void(std::uint64_t, const livekit::VideoFrame&, int)>;
// SDK must already be initialized. Shared by the desktop process and the real
// synthetic-media acceptance test; no name-based callbacks or live-map reads.
void view_screen(ViewControl& control, const ViewGrant& grant,
                 const ViewEvent& event, const ViewPixels& pixels);
}
