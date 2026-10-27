#include "view_control.h"
#include <iostream>
#include <stdexcept>
using namespace gcp::transport;
void require(bool value) { if (!value) throw std::runtime_error("viewer protocol invariant"); }
int main() {
    try {
        const auto token = std::string(24, 'a') + "." + std::string(24, 'b') + "." + std::string(24, 'c');
        const auto watch = "watch 61000 wss://screen.example.test " + token +
            " gcp-screen-00000000-0000-0000-0000-000000000001 gcp-screen-view-00000000-0000-0000-0000-000000000002 gcp-screen-publish-00000000-0000-0000-0000-000000000003";
        ViewControl valid(1000); require(valid.accept(watch, 1000)); require(valid.take_grant().has_value()); require(!valid.take_grant());
        require(!valid.take_credit()); require(valid.accept("track TR_exact", 1001)); require(valid.accept("next", 1002));
        require(valid.take_credit() && !valid.take_credit()); require(valid.accept("ping",5000)); require(valid.accept("renew 64000",5000));
        require(!valid.alive(10001)); require(!valid.accept("next",10001));
        ViewControl stopped(1000); require(stopped.accept(watch,1000)); require(stopped.accept("stop",1001)); require(!stopped.accept("renew 61000",1002));
        ViewControl duplicate(1000); require(duplicate.accept(watch,1000)); require(duplicate.accept("track TR_exact",1001));
        require(duplicate.accept("next",1002)); require(!duplicate.accept("next",1003));
        ViewControl expiry(1000); require(expiry.accept(watch,1000));
        for (int now = 5000; now < 61000; now += 4000) require(expiry.accept("ping",now));
        require(!expiry.accept("renew 100000",61000));
        for (const auto& line : {std::string("next"), std::string("track TR_wrong"), watch + " ", std::string(15000,'x'), std::string("renew 61000")}) {
            ViewControl bad(1000); require(!bad.accept(line,1000) && !bad.alive(1001));
        }
        auto frame = view_frame_header("frame 1 160 88 0 56320"); require(frame && frame->width == 160 && frame->height == 88);
        require(view_frame_header("frame 2 1920 1080 270 8294400").has_value());
        for (const auto* line : {"frame 0 160 88 0 56320", "frame 1 160 88 45 56320", "frame 1 160 88 0 56319", "frame 1 4000000000 4000000000 0 0", "frame 1 1921 1080 0 8298720", "frame 1 160 88 0 56320 trailing", "frame -1 160 88 0 56320"}) require(!view_frame_header(line));
        require(!screen_track_sid("TR_bad name") && !screen_track_sid("TR_") && screen_track_sid("TR_exact"));
        std::cout << "Viewer role, exact-SID, credit, frame bounds, heartbeat, expiry and terminal-stop policy passed; no SDK/media.\n";
    } catch (const std::exception&) { return 1; }
}
