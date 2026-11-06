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
static xr::HandState At(const XrPosef& head,float side,float forward,float up,bool down=false){
    const auto f=controls::Rot(head.orientation,{0,0,-1});
    const auto r=controls::Rot(head.orientation,{1,0,0});
    xr::HandState hand{};hand.active=hand.gripValid=hand.aimValid=true;
    hand.grip.position={head.position.x+r.x*side+f.x*forward,head.position.y+up,head.position.z+r.z*side+f.z*forward};
    hand.grip.orientation=down?XrQuaternionf{-.70710678f,0,0,.70710678f}:head.orientation;
    hand.aim=hand.grip;return hand;
}
static void Holsters(){
    constexpr WORD slots=XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_UP;
    for(bool left:{false,true}){
        const int weapon=left?0:1,support=1-weapon;const float side=left?-.3f:.3f;
        Reset(left);
        for(int degree=-180;degree<=180;degree+=30){
            const float angle=degree*.01745329252f;
            XrPosef head{{0,std::sin(angle/2),0,std::cos(angle/2)},{3,1.65f,-7}};
            Check(controls::WeaponHandZone(At(head,side,-.15f,-.1f),head)==controls::Z_SHOULDER,"weapon-side shoulder follows head yaw in both layouts");
            Check(controls::WeaponHandZone(At(head,-side,-.15f,-.1f),head)==controls::Z_NONE,"opposite shoulder is not a holster");
            Check(controls::WeaponHandZone(At(head,side,0,-.65f,true),head)==controls::Z_HIP,"weapon-side hip follows head yaw in both layouts");
            Check(controls::WeaponHandZone(At(head,-side,0,-.65f,true),head)==controls::Z_NONE,"opposite hip is not a holster");
            Check(controls::WeaponHandZone(At(head,side,0,-.65f),head)==controls::Z_NONE,"hip still requires downward pointing");
            Check(controls::WeaponHandZone(At(head,0,.15f,-.25f),head)==controls::Z_CHEST,"chest slot stays centred in both layouts");
        }
        XrPosef head{{0,0,0,1},{0,1.65f,0}};
        for(auto zone:{controls::Z_SHOULDER,controls::Z_HIP,controls::Z_CHEST}){
            Reset(left);controls::g_shoulderNext=0;
            xr::HandState h[2]={At(head,-.25f,.4f,-.25f),At(head,.25f,.4f,-.25f)};
            h[weapon]=zone==controls::Z_SHOULDER?At(head,side,-.15f,-.1f):zone==controls::Z_HIP?At(head,side,0,-.65f,true):At(head,0,.15f,-.25f);
            h[weapon].squeeze=.9f;XINPUT_GAMEPAD pad{};
            controls::Build(pad,h,head,true);
            const WORD expected=zone==controls::Z_SHOULDER?XINPUT_GAMEPAD_DPAD_LEFT:zone==controls::Z_HIP?XINPUT_GAMEPAD_DPAD_RIGHT:XINPUT_GAMEPAD_DPAD_UP;
            Check((pad.wButtons&slots)==expected,"real mapper selects the matching shoulder/hip/chest weapon slot");
            Check(controls::TakeWeaponSelect()==1 && controls::WeaponHandInHolsterZone(),"holster select and interaction exclusion follow weapon role");
            Check(lastHaptic==weapon,"holster haptic follows the physical weapon hand");
            controls::Build(pad,h,head,true);Check(controls::TakeWeaponSelect()==0,"holding grip does not select repeatedly");
            controls::CancelPendingInput();
            Check(!controls::WeaponHandInHolsterZone() && controls::TakeWeaponSelect()==0,"F1 reset clears old zone and pending selection");
            for(const auto& pulse:controls::g_pulse)Check(!pulse.btn && !pulse.until,"F1 reset clears old holster slot pulses");
        }
        Reset(left);controls::g_shoulderNext=1;
        xr::HandState h[2]={At(head,-.25f,.4f,-.25f),At(head,.25f,.4f,-.25f)};XINPUT_GAMEPAD pad{};
        h[weapon]=At(head,side,-.15f,-.1f);h[weapon].squeeze=.9f;controls::Build(pad,h,head,true);
        Check((pad.wButtons&slots)==XINPUT_GAMEPAD_DPAD_DOWN,"shoulder still alternates to special weapon in both modes");
        for(bool hip:{false,true}){
            Reset(left);h[weapon]=At(head,-side,hip?0:-.15f,hip?-.65f:-.1f,hip);h[weapon].squeeze=.9f;
            controls::Build(pad,h,head,true);Check(!(pad.wButtons&slots) && !controls::TakeWeaponSelect(),"wrong-side reach cannot select a weapon");
            Reset(left);h[weapon]=At(head,side,hip?0:-.15f,hip?-.65f:-.1f,hip);h[weapon].squeeze=.9f;
            controls::Build(pad,h,head,false);Check(!(pad.wButtons&slots) && !controls::TakeWeaponSelect(),"head tracking loss blocks holster selection");
            Reset(left);h[weapon].gripValid=false;
            controls::Build(pad,h,head,true);Check(!(pad.wButtons&slots) && !controls::TakeWeaponSelect(),"weapon grip tracking loss blocks holster selection");
        }
        Reset(left);h[weapon]=At(head,side,.4f,-.25f);h[support]=At(head,side,-.15f,-.1f);h[support].squeeze=.9f;
        controls::Build(pad,h,head,true);Check(!(pad.wButtons&slots) && !controls::TakeWeaponSelect(),"support hand cannot activate the weapon holster");
        h[support].squeeze=0;
        for(int map:{0,2}){ // native driver/passenger action maps via the existing zero-base engine stub
            Reset(left);controls::g_maps[map].id=0;
            h[weapon]=At(head,side,-.15f,-.1f);h[weapon].squeeze=.9f;
            controls::Build(pad,h,head,true);Check(!(pad.wButtons&slots) && !controls::TakeWeaponSelect(),"vehicle seats still block holster selection");
        }
    }
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
    Holsters();
    fc2vr::hands::Configure(false);
    std::puts("PASS: actual mapper, fire/reload/pose gates/sticks/buttons/haptics and mirrored shoulder/hip holsters across hand layouts/head yaw");
}
