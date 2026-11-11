#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace controls {
static uintptr_t g_base=1;
struct NativeSteeringOwner { uintptr_t pawn=0,vehicle=0,physics=0,havok=0,component=0,analog=0; };
struct Fixture {
    alignas(4) uint8_t pawn[0x20],pawnHandle[0x10],pawnEntity[0xA0];
    alignas(4) uint8_t vehicle[0x20],vehicleHandle[0x10],entity[0xA0],data[0x20],seats[0x50];
    alignas(4) uint8_t comp[0x70],wrappers[4],wrapper[0x60],physics[0x188],havok[0xD8],component[0x40],analog[0x10];
    uintptr_t pawnVtable[32],vehicleVtable[1],entityVtable[1],compVtable[1];
    uintptr_t wrapperVtable[0x138/4],physicsVtable[0x140/4],havokVtable[1],steeringVtable[5];
    uintptr_t vehicleType[6],wheeledType[7];
};
static Fixture f;
static unsigned currentCalls=0,queryCalls=0,driverCalls=0,wrapperCalls=0;
static bool nullCurrent=false,nullQuery=false,wrongWrapper=false;
template<class T> static uintptr_t Addr(T& a) { return (uintptr_t)&a; }
static void Put(uint8_t* p,size_t offset,uintptr_t v) { std::memcpy(p+offset,&v,4); }
static uintptr_t Read(const uint8_t* p,size_t offset) { uintptr_t v;std::memcpy(&v,p+offset,4);return v; }
static uintptr_t __fastcall PawnMethod(void*,void*) { return 0; }
static uintptr_t __cdecl Current(void* pawn) { ++currentCalls;return !nullCurrent && pawn==f.pawn?Addr(f.vehicle):0; }
static uintptr_t __fastcall Query(void* entity,void*,const void* type) {
    ++queryCalls;return !nullQuery && entity==f.entity && type==f.wheeledType+6?Addr(f.comp):0;
}
static uintptr_t __fastcall Wrapper(void* comp,void*) {
    ++wrapperCalls;return comp==f.comp&&!wrongWrapper?Read(f.wrappers,0):0;
}
static uint32_t* __fastcall Driver(void* vehicle,void*,uint32_t* out,int flag) {
    ++driverCalls;
    if(vehicle==f.vehicle && flag==0) { out[0]=(uint32_t)Read(f.seats,0x28);out[1]=(uint32_t)Read(f.seats,0x2C); }
    return out;
}
static void Calculate() {}
static uintptr_t NGA(uintptr_t va) {
    switch(va) {
    case 0x100E6160:return (uintptr_t)Current;
    case 0x104D5530:return (uintptr_t)Query;
    case 0x10068D10:return (uintptr_t)Wrapper;
    case 0x100D7910:return (uintptr_t)Driver;
    case 0x104B9C80:return (uintptr_t)Calculate;
    case 0x10F2F07C:return Addr(f.vehicleType);
    case 0x10F2D960:return Addr(f.wheeledType);
    case 0x10F2D978:return Addr(f.wheeledType[6]);
    case 0x10DDD6C0:return Addr(f.wrapperVtable);
    case 0x10DDF088:return Addr(f.physicsVtable);
    default:return 0;
    }
}
#include "../src/controls_vehicle_owner.inl"
static void ResetFixture() {
    f={};currentCalls=queryCalls=driverCalls=wrapperCalls=0;
    nullCurrent=nullQuery=wrongWrapper=false;g_nativeSteeringOwnerReady=true;
    f.pawnVtable[31]=(uintptr_t)PawnMethod;
    f.vehicleVtable[0]=f.entityVtable[0]=f.compVtable[0]=f.havokVtable[0]=(uintptr_t)Calculate;
    f.wrapperVtable[2]=(uintptr_t)Calculate;f.steeringVtable[4]=(uintptr_t)Calculate;
    f.vehicleType[0]=f.wheeledType[0]=1;f.wheeledType[6]=0x12345678;
    Put(f.pawn,0,Addr(f.pawnVtable));Put(f.pawn,8,Addr(f.pawnHandle));
    Put(f.pawnHandle,0,123);Put(f.pawnHandle,4,456);Put(f.pawnHandle,12,Addr(f.pawnEntity));
    Put(f.pawnEntity,8,123);Put(f.pawnEntity,12,456);
    Put(f.vehicle,0,Addr(f.vehicleVtable));Put(f.vehicle,8,Addr(f.vehicleHandle));Put(f.vehicle,16,Addr(f.data));
    Put(f.vehicleHandle,0,789);Put(f.vehicleHandle,4,321);Put(f.vehicleHandle,12,Addr(f.entity));
    Put(f.entity,0,Addr(f.entityVtable));Put(f.entity,8,789);Put(f.entity,12,321);
    Put(f.data,12,Addr(f.seats));Put(f.data,16,1);Put(f.seats,16,1);Put(f.seats,0x28,123);Put(f.seats,0x2C,456);
    Put(f.comp,0,Addr(f.compVtable));Put(f.comp,0x60,Addr(f.wrappers));Put(f.comp,0x64,1);
    Put(f.wrappers,0,Addr(f.wrapper));Put(f.wrapper,0,Addr(f.wrapperVtable));
    Put(f.wrapper,0x54,Addr(f.physics));Put(f.wrapper,0x5C,Addr(f.physics));
    Put(f.physics,0,Addr(f.physicsVtable));Put(f.physics,0x180,Addr(f.wrapper));Put(f.physics,0x184,Addr(f.havok));
    Put(f.havok,0,Addr(f.havokVtable));Put(f.havok,0xD4,Addr(f.physics));
    Put(f.havok,0x24,Addr(f.component));Put(f.havok,0x9C,Addr(f.analog));
    Put(f.component,0,Addr(f.steeringVtable));
}
}
int main() {
    using namespace controls;
    static_assert(sizeof(uintptr_t)==4,"production adapter requires Win32 test");
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name) {++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",name);}};
    auto empty=[](const NativeSteeringOwner& o) {return !o.pawn&&!o.vehicle&&!o.physics&&!o.havok&&!o.component&&!o.analog;};
    auto rejected=[&](auto mutate,const char* name) {
        ResetFixture();mutate();const Fixture before=f;
        check(empty(ResolveSteeringOwner(f.pawn)),name);
        check(std::memcmp(&f,&before,sizeof f)==0,"rejected chain preserves borrowed bytes");
    };
    ResetFixture();const Fixture before=f;const auto owner=ResolveSteeringOwner(f.pawn);
    check(owner.pawn==Addr(f.pawn)&&owner.vehicle==Addr(f.vehicle)&&owner.physics==Addr(f.physics)
        &&owner.havok==Addr(f.havok)&&owner.component==Addr(f.component)&&owner.analog==Addr(f.analog),"complete native chain identifies exact owner");
    check(currentCalls==1&&driverCalls==1&&queryCalls==1&&wrapperCalls==1,"production resolver calls the expected ABI adapters once");
    check(std::memcmp(&f,&before,sizeof f)==0,"valid owner leaves all borrowed object bytes unchanged");
    rejected([]{g_nativeSteeringOwnerReady=false;},"unverified native code refuses resolution");
    check(currentCalls==0,"disabled resolver never invokes native getter");
    ResetFixture();check(empty(ResolveSteeringOwner(nullptr)),"null pawn rejected");
    rejected([]{Put(f.pawn,0,0);},"missing pawn vtable");
    rejected([]{Put(f.pawn,8,0);},"missing pawn handle");
    rejected([]{Put(f.pawnHandle,12,0);},"missing pawn entity");
    rejected([]{Put(f.pawnEntity,8,999);},"pawn low entity identity mismatch");
    rejected([]{Put(f.pawnEntity,12,999);},"pawn high entity identity mismatch");
    rejected([]{Put(f.pawnHandle,0,0xFFFFFFFF);Put(f.pawnHandle,4,0xFFFFFFFF);},"invalid pawn entity ID");
    rejected([]{f.vehicleType[0]=0;},"vehicle registry not initialized");
    rejected([]{f.wheeledType[0]=0;},"wheeled registry not initialized");
    rejected([]{nullCurrent=true;},"no current vehicle");
    rejected([]{Put(f.vehicle,16,0);},"missing vehicle data");
    rejected([]{Put(f.data,12,0);},"missing seat array");
    rejected([]{Put(f.data,16,0);},"empty seat array");
    rejected([]{Put(f.data,16,33);},"oversized seat array");
    rejected([]{Put(f.seats,0x28,999);},"driver low ID mismatch");
    check(queryCalls==0,"wrong driver cannot reach physics query");
    rejected([]{Put(f.seats,0x2C,999);},"driver high ID mismatch");
    rejected([]{Put(f.vehicle,8,0);},"missing vehicle handle");
    rejected([]{Put(f.vehicleHandle,12,0);},"missing vehicle entity");
    rejected([]{Put(f.entity,8,999);},"vehicle low entity ID mismatch");
    rejected([]{Put(f.entity,12,999);},"vehicle high entity ID mismatch");
    rejected([]{nullQuery=true;},"vehicle lacks wheeled physics component");
    rejected([]{Put(f.comp,0x60,0);},"missing physics wrapper array");
    rejected([]{Put(f.comp,0x64,0);},"empty physics wrapper array");
    rejected([]{Put(f.comp,0x64,65);},"oversized physics wrapper array");
    rejected([]{Put(f.wrappers,0,0);},"null wrapper");
    rejected([]{Put(f.wrapper,0,Addr(f.compVtable));},"nonwheeled wrapper vtable");
    rejected([]{wrongWrapper=true;},"native getter disagrees with wrapper identity");
    rejected([]{Put(f.wrapper,0x5C,0);},"missing physics implementation");
    rejected([]{Put(f.physics,0,Addr(f.compVtable));},"wrong physics implementation vtable");
    rejected([]{Put(f.wrapper,0x54,0);},"wrapper base pointer disagreement");
    rejected([]{Put(f.physics,0x180,0);},"physics wrapper backlink mismatch");
    rejected([]{Put(f.physics,0x184,0);},"missing Havok instance");
    rejected([]{Put(f.havok,0xD4,0);},"Havok physics backlink mismatch");
    rejected([]{Put(f.havok,0x24,0);},"missing steering component");
    rejected([]{Put(f.havok,0x9C,0);},"missing analog input status");
    rejected([]{Put(f.component,0,0);},"missing steering vtable");
    rejected([]{f.steeringVtable[4]=(uintptr_t)PawnMethod;},"unexpected steering implementation");
    g_base=0;check(!InitNativeSteeringOwner(),"installation without image fails closed");
    std::printf("native vehicle owner: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
