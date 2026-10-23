#include "screen_control.h"
#include <iostream>
#include <stdexcept>
using namespace gcp::transport;
void require(bool condition) { if (!condition) throw std::runtime_error("screen control invariant"); }
int main() {
    try {
        const std::string grant = "publish 61000 wss://screen.example.test " + std::string(24, 'a') + "." +
            std::string(24, 'b') + "." + std::string(24, 'c') +
            " gcp-screen-00000000-0000-0000-0000-000000000001 gcp-screen-publish-00000000-0000-0000-0000-000000000002";
        ScreenControl unselected(1000);
        require(!unselected.accept(grant,1000));
        ScreenControl selected(1000); selected.selected();
        require(selected.accept(grant,1000));
        auto issued = selected.take_grant(); require(issued && issued->deadline == 61000);
        require(!selected.take_grant());
        require(selected.accept("ping",5000));
        require(!selected.alive(10001)); // parent loss
        require(!selected.accept("renew 70000",10001));
        ScreenControl stopped(1000); stopped.selected();
        require(stopped.accept(grant,1000)); require(stopped.accept("stop",1001));
        require(!stopped.alive(1001) && !stopped.accept("renew 61000",1002));
        ScreenControl expired(1000); expired.selected();
        require(expired.accept(grant,1000));
        for (int now=5000; now<=61000; now+=4000) {
            if (now<61000) require(expired.accept("ping",now));
        }
        require(!expired.accept("renew 120000",61000));
        ScreenControl duplicate(1000); duplicate.selected();
        require(duplicate.accept(grant,1000)); require(!duplicate.accept(grant,1001));
        ScreenControl extended(1000); extended.selected();
        require(!extended.accept("publish 999999 " + grant.substr(grant.find("wss://")),1000));
        ScreenControl renewed(1000); renewed.selected();
        require(renewed.accept(grant,1000));
        require(renewed.accept("ping",5000));
        require(renewed.accept("renew 64000",5000));
        require(renewed.accept("ping",62000) == false); // no heartbeat in between
        ScreenControl early(1000); early.selected();
        require(!early.accept("renew 61000",1000));
        for (const auto& bad : {std::string(""), std::string("unknown"), grant + " ",
             std::string("publish 999 ") + grant.substr(grant.find("wss://")),
             std::string("publish 61000 ws://remote.example ") + grant.substr(grant.find(' ', grant.find("wss://")) + 1)}) {
            ScreenControl invalid(1000); invalid.selected();
            require(!invalid.accept(bad,1000) && !invalid.alive(1001));
        }
        ScreenControl oversized(1000); oversized.selected();
        require(!oversized.accept(std::string(20000,'x'),1000));
        require(!screen_jwt("bad") && !screen_uuid("00000000-0000-0000-0000-000000000000"));
        std::cout << "Screen consent/expiry/parent-loss/stop protocol passed; no capture or network.\n";
    } catch (const std::exception&) { return 1; }
}
