#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include "../src/common/native_steering_response.h"
#include "../src/common/pose_authority.h"

namespace xr { using InputStamp=fc2vr::authority::Stamp; }
namespace arms {
static void* localPawn=nullptr;
static bool IsLocalPawn(const void* pawn) { return pawn && pawn==localPawn; }
}
namespace gameaddr {
enum Version { GOG };
static Version Detect() { return GOG; }
}
#define LOG(...) ((void)0)
namespace controls {
using PatchFn=bool (*)(uintptr_t,const uint8_t*,size_t,void*,void**);
struct NativeSteeringOwner { uintptr_t pawn=0,vehicle=0,physics=0,havok=0,component=0,analog=0; };
static struct Config { bool steeringDirect=true,motionSteering=true,motionInputTrace=false; } g_c;
static SRWLOCK g_wheelHoldLock=SRWLOCK_INIT;
static xr::InputStamp g_wheelHoldSource;
static NativeSteeringOwner g_wheelOwner;
static uint8_t g_wheelHoldMask=0;
static bool g_wheelMotionOwned=false,g_steeringResponseEnabled=false;
static float g_wheelEmittedAxis=0;
struct Fixture {
    alignas(4) uint8_t pawn[4],vehicle[4],physics[0x188],havok[0xD8],component[0x40],analog[0x10];
    uintptr_t physicsVtable[1],steeringVtable[5];
};
static Fixture f;
static xr::InputStamp fixtureCurrent;
static uint64_t nowMs=1000;
static uint8_t liveGripMask=3;
static bool liveDriving=true;
static unsigned readableCalls=0,maskCalls=0;
template<class T> static uintptr_t Addr(T& v) { return (uintptr_t)&v; }
static void Put(uint8_t* p,size_t offset,uintptr_t value) { std::memcpy(p+offset,&value,4); }
static void Calculate() {}
static uintptr_t NGA(uintptr_t address) {
    if(address==0x10DDF088) return Addr(f.physicsVtable);
    if(address==0x104B9C80) return (uintptr_t)Calculate;
    return 0;
}
static bool NativeOwnerReadable(uintptr_t p,size_t n) {
    ++readableCalls;
    return p && n && p+n>=p && !IsBadReadPtr((const void*)p,n);
}
static bool InitNativeSteeringOwner() { return false; }
static bool NativeOwnerCode(uintptr_t,const uint8_t*,size_t) { return false; }
// The callback's XR dependency is injected here: the production mask helper
// supplies driving/grip eligibility and the same real freshness policy. This
// fixture tests how the actual callback honors cancellation, not XR polling.
static uint8_t CurrentWheelMask(const xr::InputStamp& source,uint8_t mask) {
    ++maskCalls;
    if(!g_c.motionSteering || !liveDriving) return 0;
    mask&=liveGripMask;
    for(int side=0;side<2;++side)
        if(!fc2vr::authority::Fresh(source,fixtureCurrent,nowMs,side)) mask&=uint8_t(~(1u<<side));
    return mask;
}
#include "../src/controls_steering.inl"
static void ResetFixture() {
    f={};g_c={};g_steeringResponseEnabled=true;g_wheelMotionOwned=true;
    readableCalls=maskCalls=0;nowMs=1000;liveGripMask=g_wheelHoldMask=3;liveDriving=true;
    fixtureCurrent={};fixtureCurrent.sequence=5;fixtureCurrent.receivedMs=1000;fixtureCurrent.generation=3;
    fixtureCurrent.handGeneration[0]=8;fixtureCurrent.handGeneration[1]=9;fixtureCurrent.focused=fixtureCurrent.synced=true;
    g_wheelHoldSource=fixtureCurrent;g_wheelEmittedAxis=.5f;arms::localPawn=f.pawn;
    Put(f.physics,0,Addr(f.physicsVtable));Put(f.physics,0x184,Addr(f.havok));
    Put(f.havok,0xD4,Addr(f.physics));Put(f.havok,0x24,Addr(f.component));Put(f.havok,0x9C,Addr(f.analog));
    Put(f.component,0,Addr(f.steeringVtable));f.steeringVtable[4]=(uintptr_t)Calculate;
    g_wheelOwner={Addr(f.pawn),Addr(f.vehicle),Addr(f.physics),Addr(f.havok),Addr(f.component),Addr(f.analog)};
    const float axis=.5f;std::memcpy(f.analog+8,&axis,4);
}
static int Invoke(float axis=.5f,float maximum=.6f) {
    return NativeSteeringResponse(f.component,f.havok,f.analog,axis,maximum);
}
}
int main() {
    using namespace controls;
    static_assert(sizeof(uintptr_t)==4,"production callback fixture requires Win32");
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name) { ++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",name);} };
    auto rejected=[&](auto mutate,const char* name) {
        ResetFixture();mutate();const Fixture before=f;
        check(Invoke()==0,name);
        check(std::memcmp(&before,&f,sizeof f)==0,"decline does not mutate native objects");
    };
    ResetFixture();const Fixture before=f;
    check(Invoke()!=0,"exact local motion-owned steering accepted");
    check(std::memcmp(&before,&f,sizeof f)==0,"acceptance itself is read only");
    check(maskCalls==1,"accepted tuple requires current grip authority");
    rejected([]{g_steeringResponseEnabled=false;},"uninstalled hook disabled");
    rejected([]{g_c.steeringDirect=false;},"direct response setting disabled");
    rejected([]{g_c.motionSteering=false;},"motion steering setting disabled");
    rejected([]{g_wheelMotionOwned=false;},"joystick override retains native response");
    check(readableCalls==0,"joystick override rejected before native dereference");
    rejected([]{g_wheelOwner={};},"cancelled published owner rejected");
    check(readableCalls==0,"cancelled owner rejected before native dereference");
    rejected([]{arms::localPawn=f.vehicle;},"changed local pawn rejected");
    check(readableCalls==0,"changed pawn rejected before native dereference");
    ResetFixture();
    check(!NativeSteeringResponse((void*)1,f.havok,f.analog,.5f,.6f),"unrelated steering component rejected");
    check(!NativeSteeringResponse(f.component,(void*)1,f.analog,.5f,.6f),"unrelated Havok vehicle rejected");
    check(!NativeSteeringResponse(f.component,f.havok,(void*)1,.5f,.6f),"unrelated analog block rejected");
    check(readableCalls==0,"unrelated objects never dereferenced");
    rejected([]{g_wheelHoldMask=0;},"released published grips rejected");
    rejected([]{liveGripMask=0;},"current grip release cancels cached ownership");
    rejected([]{liveDriving=false;},"current mode cancellation honored");
    rejected([]{fixtureCurrent.generation++;},"XR generation changed");
    rejected([]{fixtureCurrent.handGeneration[0]++;fixtureCurrent.handGeneration[1]++;},"both hand generations changed");
    rejected([]{fixtureCurrent.focused=false;},"focus lost");
    rejected([]{fixtureCurrent.synced=false;},"action synchronization lost");
    rejected([]{nowMs=1101;},"expired published input");
    rejected([]{nowMs=999;},"future input timestamp rejected");
    rejected([]{fixtureCurrent.sequence=4;},"source ahead of current input rejected");
    rejected([]{g_wheelHoldSource.sequence=0;},"cancelled source stamp rejected");
    ResetFixture();liveGripMask=1;fixtureCurrent.handGeneration[1]++;
    check(Invoke()!=0,"one fresh held hand remains eligible");
    ResetFixture();fixtureCurrent.sequence++;fixtureCurrent.receivedMs++;nowMs++;
    check(Invoke()!=0,"newer same-context XR sample retains recent ownership");
    rejected([]{g_wheelOwner.physics=0;},"null physics object");
    rejected([]{Put(f.physics,0,0);},"wrong physics implementation");
    rejected([]{Put(f.physics,0x184,0);},"broken physics to Havok link");
    rejected([]{Put(f.havok,0xD4,0);},"broken Havok physics backlink");
    rejected([]{Put(f.havok,0x24,0);},"changed steering component");
    rejected([]{Put(f.havok,0x9C,0);},"changed native analog block");
    rejected([]{Put(f.component,0,0);},"missing steering vtable");
    rejected([]{f.steeringVtable[4]=0;},"wrong steering function");
    rejected([]{const float n=std::numeric_limits<float>::quiet_NaN();std::memcpy(f.analog+8,&n,4);},"NaN native analog axis");
    rejected([]{const float n=std::numeric_limits<float>::infinity();std::memcpy(f.analog+8,&n,4);},"infinite native analog axis");
    rejected([]{const float n=-.5f;std::memcpy(f.analog+8,&n,4);},"native analog disagrees with seam axis");
    rejected([]{g_wheelEmittedAxis=-.5f;},"opposite sign emitted input rejected");
    rejected([]{g_wheelEmittedAxis=.4f;},"different emitted input rejected");
    rejected([]{g_wheelEmittedAxis=std::numeric_limits<float>::quiet_NaN();},"NaN cached emission rejected");
    // Independent expected native normalization from the actual signed SHORT.
    // Threshold-adjacent codes distinguish /32767 from negative /32768.
    for(const int code:{-32767,-31458,-31457,-16384,-1,0,1,16384,31456,31457,32767}) {
        ResetFixture();g_wheelEmittedAxis=float(code)/32767.f;
        float nativeAxis=code<0?float(code)/32768.f:float(code)/32767.f;
        if(std::fabs(nativeAxis)>=.96f) nativeAxis=nativeAxis<0?-1.f:1.f;
        std::memcpy(f.analog+8,&nativeAxis,4);
        check(Invoke(nativeAxis)!=0,"native SHORT scaling/saturation matches published emission");
        check(!Invoke(nativeAxis<0?.5f:-.5f),"opposite native sign never accepted");
    }
    for(const float invalid:{0.f,-1.f,3.2f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        ResetFixture();check(!Invoke(.5f,invalid),"invalid native maximum falls through");
    }
    for(const float invalid:{-1.1f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        ResetFixture();check(!Invoke(invalid),"invalid seam axis falls through");
    }
    for(const float invalid:{-1.1f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        ResetFixture();g_wheelEmittedAxis=invalid;
        const float nativeAxis=invalid<0?-1.f:1.f;std::memcpy(f.analog+8,&nativeAxis,4);
        check(!Invoke(nativeAxis),"invalid cached emission cannot become a valid saturated axis");
    }
    InstallMotionSteeringResponse(nullptr); // Compiles production installer; performs no native patch.
    std::printf("steering owner response: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
