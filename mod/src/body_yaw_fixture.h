#pragma once
// Explicit native-test DLL only. Synthetic HEAD poses do not authorize real
// controller actions. This adapter is absent from the production DLL.
#if defined(FC2VR_BODY_YAW_TESTING)
#include "common/hand_roles.h"
namespace body_yaw_fixture {
struct State {
    bool configured=false,tracked=false,allowed=false;
    unsigned long long sequence=0,received=0,reset=0;
    float yaw=0,pitch=0,roll=0;
};
inline State current;
// Visual-only controller targets for anatomical left-weapon readback. They do
// not enter InputSnapshot, publish input stamps or authorize controller actions.
inline bool VisualHand(int physical,xr::HandState& hand) {
    if(!current.configured)return false;
    hand={};hand.gripValid=hand.aimValid=true;
    hand.grip.position={physical?0.25f:-0.25f,1.35f,-0.4f};
    const auto phase=current.sequence/100000;
    if(phase>=30 && phase<180 && ((phase-30)%5==2 || (phase-30)%5==3) && physical==fc2vr::hands::Support())
        hand.grip.position={physical?.15f:-.15f,1.4f,-.65f};
    hand.aim=hand.grip;
    return true;
}
inline void Update(xr::FrameViews& views,bool begun) {
    static char path[MAX_PATH]{};static bool init=false;
    if(!init){init=true;const DWORD length=GetEnvironmentVariableA("FC2VR_BODY_YAW_TEST_FILE",path,MAX_PATH);current.configured=length>0 && length<MAX_PATH;}
    if(!current.configured || !begun)return;
    current.tracked=current.allowed=false;
    HANDLE f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f!=INVALID_HANDLE_VALUE){
        char line[256]{};DWORD count=0,pid=0;unsigned long long seq=0,reset=0;float yaw=0,pitch=0,roll=0;int tracked=0,allowed=0;
        if(ReadFile(f,line,sizeof(line)-1,&count,nullptr) && sscanf(line,"%lu,%llu,%f,%f,%f,%d,%d,%llu",&pid,&seq,&yaw,&pitch,&roll,&tracked,&allowed,&reset)==8 && pid==GetCurrentProcessId() && seq && std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll)){
            if(seq!=current.sequence){current.sequence=seq;current.received=GetTickCount64();}
            current.tracked=tracked && GetTickCount64()-current.received<=100;
            current.allowed=allowed!=0;current.reset=reset;
            current.yaw=yaw;current.pitch=pitch;current.roll=roll;
        }
        CloseHandle(f);
    }
    const float factor=3.14159265358979323846f/360;
    const float sy=sinf(current.yaw*factor),cy=cosf(current.yaw*factor),sp=sinf(current.pitch*factor),cp=cosf(current.pitch*factor),sr=sinf(current.roll*factor),cr=cosf(current.roll*factor);
    const XrQuaternionf q{cy*sp*cr+sy*cp*sr,sy*cp*cr-cy*sp*sr,cy*cp*sr-sy*sp*cr,cy*cp*cr+sy*sp*sr};
    for(int eye=0;eye<2;++eye){
        const float side=eye?0.032f:-0.032f;
        views.views[eye].pose.orientation=q;
        views.views[eye].pose.position={side*cosf(current.yaw*factor*2),1.65f,-side*sinf(current.yaw*factor*2)};
    }
    views.valid=views.positionValid=views.shouldRender=true;
}
}
#endif
