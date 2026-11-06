// Execute the real mapper without a game, OpenXR session, input injection or
// engine hooks. Including the implementation keeps test seams out of the DLL.
#include "../src/controls.cpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int lastHaptic=-1;
void LogWrite(const char*,...) {}
namespace gameaddr { uint32_t Xlate(uint32_t) { return 0; } }
namespace xr {
bool InputSnapshot(HandState[2],XrPosef&,bool&,InputStamp*) { return false; }
void Haptic(int hand,float,float,float) { lastHaptic=hand; }
void SetRumble(float,float) {}
}
static void Check(bool ok,const char* label) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}
}
static void Reset(bool left) {
    fc2vr::hands::Configure(left);
    controls::CancelPendingInput();
    controls::g_c=controls::Config{};
    controls::g_c.snapTurn=0;controls::g_c.throwArc=0;controls::g_c.moveDir=0;
    controls::g_cycle=0;controls::g_mono=false;controls::g_mapOpen=false;
    controls::g_menuMode=false;controls::g_sightOn=false;
    // A zero engine base means no native access. Avoid matching the uninitialized
    // action-map IDs to the zero (unknown) result in this isolated test.
    unsigned id=1;for(auto& map:controls::g_maps)map.id=id++;
    lastHaptic=-1;
}
int main() {
    for(bool left:{false,true}) {
        const int weapon=left?0:1,support=1-weapon;
        xr::HandState h[2];XrPosef head{{0,0,0,1},{0,1.65f,0}};
        for(auto& hand:h){hand.active=true;hand.gripValid=hand.aimValid=true;hand.grip.position={0,1.4f,-0.4f};}
        XINPUT_GAMEPAD pad{};
        Reset(left);h[weapon].trigger=0.8f;
        controls::Build(pad,h,head,true);
        Check(pad.bRightTrigger==204,"weapon trigger fires");
        Check(!(pad.wButtons&XINPUT_GAMEPAD_X),"weapon trigger does not reload");
        Reset(left);h[weapon].trigger=0;h[support].trigger=0.8f;
        controls::Build(pad,h,head,true);
        Check(pad.bRightTrigger==0,"support trigger cannot fire");
        Check((pad.wButtons&XINPUT_GAMEPAD_X)!=0,"support trigger reloads");
        Reset(left);h[support].trigger=0;h[weapon].trigger=1;h[weapon].aimValid=false;
        controls::Build(pad,h,head,true);
        Check(pad.bRightTrigger==0,"untracked weapon cannot fire with a tracked support hand");
        h[weapon].aimValid=true;h[weapon].trigger=0;
        Reset(left);h[0].stick={0.3f,0.7f};h[1].stick={-0.8f,0.1f};
        controls::Build(pad,h,head,true);
        Check(pad.sThumbLX==controls::Axis(0.3f) && pad.sThumbLY==controls::Axis(0.7f),"physical left stick moves");
        Check(pad.sThumbRX==-32767,"physical right stick turns");
        h[0].stick=h[1].stick={0,0};
        Reset(left);h[0].btn1=true;
        controls::Build(pad,h,head,false);
        Check((pad.wButtons&XINPUT_GAMEPAD_Y)!=0 && !(pad.wButtons&XINPUT_GAMEPAD_A),"physical X keeps original dialog/interact mapping");
        h[0].btn1=false;
        Reset(left);h[1].btn1=true;
        controls::Build(pad,h,head,false);
        Check((pad.wButtons&XINPUT_GAMEPAD_A)!=0 && !(pad.wButtons&XINPUT_GAMEPAD_Y),"physical A keeps original mapping");
        h[1].btn1=false;
        Reset(left);h[0].menu=true;
        controls::Build(pad,h,head,false);
        Check((pad.wButtons&XINPUT_GAMEPAD_START)!=0,"pause stays on physical menu button");
        h[0].menu=false;
        Reset(left);h[weapon].grip.position=head.position;
        controls::SetLeftGripClaimed(true);
        controls::Build(pad,h,head,true);
        Check(pad.bLeftTrigger==255,"sights use weapon-to-head distance and support claim");
        Check(lastHaptic==weapon,"sights haptic reaches physical weapon hand");
    }
    fc2vr::hands::Configure(false);
    std::puts("PASS: actual gamepad mapper, both weapon hands, fire/reload/pose gates/sticks/pause/sights/haptics");
}
