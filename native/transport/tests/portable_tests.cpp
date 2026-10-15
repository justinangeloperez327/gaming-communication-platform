#include "h264_access_unit.h"
#include "annexb_h264.h"
#include "frame_arrival_metrics.h"
#include <iostream>
int main() {
    if (!gcp::transport::h264_access_unit_self_test() ||
        !gcp::transport::test_annexb_h264_parser() ||
        !gcp::transport::frame_arrival_metrics_self_test()) return 1;
    std::cout << "H.264 parsers and timing tests passed\n";
}
