#include "common/driving_controls.h"
#include <cstdio>
int main() {
    using fc2vr::input::DrivingLayout;
    unsigned checks=0,failures=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",why);}};
    // Independent truth table: motion, A-brake, expected action for A/click.
    struct Layout { bool motion,aBrake,aBrakes,aSeat,clickBrakes,clickSeat,recenter; };
    const Layout layouts[]={{false,false,false,true,false,false,true},
        {false,true,true,false,false,true,false},{true,false,false,true,true,false,false},
        {true,true,true,false,false,true,false}};
    for(const auto& l:layouts) {
        auto idle=DrivingLayout(l.motion,l.aBrake,false,false,0,0);
        check(!idle.handbrake&&!idle.changeSeat,"idle produces no native action");
        check(idle.allowRecenter==l.recenter,"recenter does not conflict with remapped stick click");
        for(int frame=0;frame<4;++frame) {
            auto a=DrivingLayout(l.motion,l.aBrake,false,true,0,0);
            check(a.handbrake==l.aBrakes&&a.changeSeat==l.aSeat,"A holds the selected action on repeated frames");
            auto click=DrivingLayout(l.motion,l.aBrake,true,false,0,0);
            check(click.handbrake==l.clickBrakes&&click.changeSeat==l.clickSeat,"click uses selected layout");
        }
        auto grips=DrivingLayout(l.motion,l.aBrake,false,false,1,1);
        check(grips.handbrake==!l.motion&&grips.changeSeat==!l.motion,"motion mode reserves grips; off preserves original grip actions");
        auto boundary=DrivingLayout(l.motion,l.aBrake,false,false,.5f,.6f);
        check(!boundary.handbrake&&!boundary.changeSeat,"original strict grip thresholds retained");
        auto release=DrivingLayout(l.motion,l.aBrake,false,false,0,0);
        check(!release.handbrake&&!release.changeSeat,"button release clears auxiliary actions");
    }
    std::printf("driving layout: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
