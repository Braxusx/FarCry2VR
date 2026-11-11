#include "common/diagnostic_gate.h"
#include <cstdio>
#include <limits>
int main() {
    fc2vr::diagnostics::ArmedGate gate;
    int bad=0;
    auto check=[&](bool ok,const char* message) { if(!ok) { ++bad; std::fprintf(stderr,"FAIL: %s\n",message); } };
    // Replay the failure: normal controls and an inventory pointer during load,
    // but no stable identified weapon. Neither may consume a one-shot capture.
    for (int t=0;t<=2000;t+=10) check(gate.Update(t,true,1,0)==0,"unknown loading model stays ineligible");
    for (int t=2010;t<2800;t+=10) check(gate.Update(t,true,1,7)<1500,"transient equipped model cannot trigger");
    check(gate.Update(2800,true,0,0)==0,"empty jeep hands reset gate");
    for (int t=3000;t<=10000;t+=10) check(gate.Update(t,true,2,8)==t-3000,"real stable weapon advances gate");
    check(gate.Update(10010,false,2,8)==0,"pause closes readiness");
    check(gate.Update(10020,true,2,8)==0,"resume starts new readiness window");
    check(gate.Update(10030,true,3,8)==0,"same model on different instance resets");
    check(gate.Update(10040,true,3,9)==0,"model identity change resets");
    check(gate.Update(11000,true,3,9)==0,"loading gap resets");
    check(gate.Update(10900,true,3,9)==0,"clock regression resets");
    check(gate.Update(std::numeric_limits<double>::quiet_NaN(),true,3,9)==0,"invalid clock resets");
    return bad ? 1 : 0;
}
