#include "common/native_steering_response.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// Execute the production x86 stub without game assets/processes. A destructive
// callback verifies the ABI isolation; both native continuation choices run.
static_assert(sizeof(void*)==4,"native steering response execution requires Win32 x86");
static int checks=0,failures=0;
static void Check(bool ok,const char* message) {
    ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",message); }
}
static std::uint32_t Address(const void* p) { return std::uint32_t(reinterpret_cast<std::uintptr_t>(p)); }
struct alignas(16) Fx { unsigned char bytes[512]{}; };
struct State {
    Fx host,before,expected,after;
    alignas(16) std::uint32_t patterns[8][4]{};
    std::uint32_t component[16]{},vehicle[56]{},analogue[8]{},frame[6]{};
    std::uint32_t registers[8]{},beforeSp=0,afterSp=0,beforeFlags=0,afterFlags=0;
    std::uint32_t seenComponent=0,seenVehicle=0,seenAnalogue=0,seenAxis=0,seenMaximum=0,calls=0,accepted=0;
    std::uint32_t inputMxcsr=0x3f80,callbackMxcsr=0x5f80;
    std::uint16_t inputControl=0x077f,callbackControl=0x0b7f;
    float output=123.f;
};
struct Code {
    std::uint32_t base;std::vector<std::uint8_t> bytes;
    explicit Code(void* address):base(Address(address)) {}
    void Emit(std::initializer_list<std::uint8_t> values) { bytes.insert(bytes.end(),values); }
    void Word(std::uint32_t value) { for(unsigned i=0;i<4;++i) bytes.push_back(std::uint8_t(value>>(8*i))); }
    void Absolute(std::initializer_list<std::uint8_t> op,const void* address) { Emit(op);Word(Address(address)); }
    void Immediate(unsigned reg,std::uint32_t value) { Emit({std::uint8_t(0xb8+reg)});Word(value); }
    void Store(unsigned reg,const void* address) { Absolute({0x89,std::uint8_t(0x05+8*reg)},address); }
    void Branch(std::uint8_t op,std::uint32_t target) { Emit({op});Word(target-(base+std::uint32_t(bytes.size())+4)); }
};
static void Write(void* address,const Code& code) { std::memcpy(address,code.bytes.data(),code.bytes.size()); }
static void Exercise(unsigned padding,unsigned result,float axis,float maximum) {
    State state;
    const float initial=.123f;
    std::memcpy(&state.component[7],&initial,4);
    std::memcpy(&state.analogue[2],&axis,4);
    state.vehicle[9]=Address(state.component);state.vehicle[39]=Address(state.analogue);
    state.frame[4]=Address(state.analogue);state.frame[5]=Address(&state.output);
    for(unsigned r=0;r<8;++r) for(unsigned lane=0;lane<4;++lane)
        state.patterns[r][lane]=0x10203040u+r*0x01010101u+lane*0x11111111u;
    std::memcpy(&state.patterns[2][0],&maximum,4);
    std::memcpy(&state.patterns[3][0],&axis,4);
    state.patterns[5][0]=0x7fffffffu; // native scalar absolute mask, zero upper lanes
    for(unsigned lane=1;lane<4;++lane) state.patterns[5][lane]=0;
    auto* memory=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(memory!=nullptr,"executable harness allocation");if(!memory) return;
    auto* stub=memory+1024;auto* callback=memory+1536;
    auto* nativeResume=memory+2048;auto* acceptedResume=memory+2560;

    Code call(callback);
    call.Emit({0x8b,0x44,0x24,0x04});call.Store(0,&state.seenComponent);
    call.Emit({0x8b,0x54,0x24,0x08});call.Store(2,&state.seenVehicle);
    call.Emit({0x8b,0x4c,0x24,0x0c});call.Store(1,&state.seenAnalogue);
    call.Emit({0x8b,0x44,0x24,0x10});call.Store(0,&state.seenAxis);
    call.Emit({0x8b,0x44,0x24,0x14});call.Store(0,&state.seenMaximum);
    call.Absolute({0xff,0x05},&state.calls);
    call.Emit({0xdb,0xe3,0xd9,0xee}); // fninit; fldz
    call.Absolute({0xd9,0x2d},&state.callbackControl);
    call.Absolute({0x0f,0xae,0x15},&state.callbackMxcsr);
    for(unsigned r=0;r<8;++r) call.Emit({0x66,0x0f,0x76,std::uint8_t(0xc0+9*r)});
    call.Immediate(1,0xdead1234);call.Immediate(2,0xbaad5678);
    call.Immediate(0,result);call.Emit({0x09,0xc0,0xc3});Write(callback,call);

    for(unsigned accepted=0;accepted<2;++accepted) {
        auto* target=accepted?acceptedResume:nativeResume;Code resume(target);
        for(unsigned r=0;r<8;++r) resume.Store(r,&state.registers[r]);
        resume.Emit({0x9c});resume.Absolute({0x8f,0x05},&state.afterFlags);
        resume.Absolute({0x0f,0xae,0x05},&state.after);
        resume.Immediate(0,accepted);resume.Store(0,&state.accepted);
        if(accepted) {
            // Existing native writeback: synchronize stored timed state and
            // the angle returned to native physics/animation, including x87.
            resume.Emit({0x8b,0x55,0x14,0xf3,0x0f,0x11,0x46,0x1c,0xd9,0x46,0x1c,0xd9,0x1a});
        }
        resume.Emit({0xc3});Write(target,resume);
    }
    const auto generated=fc2vr::motion::MakeSteeringResponseStub(Address(stub),Address(callback),
        Address(nativeResume),Address(acceptedResume));
    Check(generated.size<=generated.bytes.size(),"production stub stays within its allocation");
    std::memcpy(stub,generated.bytes.data(),generated.size);
    Code run(memory);
    run.Emit({0x9c,0x60});run.Absolute({0x0f,0xae,0x05},&state.host);
    for(unsigned n=0;n<padding;++n) { run.Emit({0x68});run.Word(0xfeed0000+n); }
    run.Emit({0xdb,0xe3,0xd9,0xe8,0xd9,0xeb,0xd9,0xed,0xd9,0xee}); // four occupied x87 slots
    run.Absolute({0xd9,0x2d},&state.inputControl);
    run.Absolute({0x0f,0xae,0x15},&state.inputMxcsr);
    for(unsigned r=0;r<8;++r) run.Absolute({0xf3,0x0f,0x6f,std::uint8_t(0x05+8*r)},&state.patterns[r]);
    const std::array<std::uint32_t,8> expected={0x11223344,0xaabbccdd,0x12341234,0x55667788,0,
        Address(state.frame),Address(state.component),Address(state.vehicle)};
    for(unsigned r=0;r<8;++r) if(r!=4) run.Immediate(r,expected[r]);
    run.Emit({0x68});run.Word(0x246u|(padding&1?0x881u:0x14u));run.Emit({0x9d});
    run.Store(4,&state.beforeSp);
    run.Emit({0x9c});run.Absolute({0x8f,0x05},&state.beforeFlags);
    run.Absolute({0x0f,0xae,0x05},&state.before);
    // Independent expected FP image accounts for intended arithmetic's MXCSR
    // precision flag without hiding any callback damage to rounding controls.
    if(result) run.Emit({0xf3,0x0f,0x10,0xc3,0xf3,0x0f,0x59,0xc2});
    else run.Emit({0x0f,0x28,0xe3,0x0f,0x54,0xe5});
    run.Absolute({0x0f,0xae,0x05},&state.expected);
    run.Absolute({0x0f,0xae,0x0d},&state.before);
    run.Branch(0xe8,Address(stub));
    run.Store(4,&state.afterSp);run.Absolute({0x0f,0xae,0x0d},&state.host);
    if(padding) run.Emit({0x83,0xc4,std::uint8_t(4*padding)});
    run.Emit({0x61,0x9d,0xc3});Write(memory,run);
    DWORD prior=0;const bool executable=VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&prior)!=0;
    Check(executable,"harness becomes executable and read-only");
    if(executable) {
        FlushInstructionCache(GetCurrentProcess(),memory,4096);reinterpret_cast<void(*)()>(memory)();
        Check(state.calls==1,"callback executes exactly once");
        Check(state.seenComponent==Address(state.component)&&state.seenVehicle==Address(state.vehicle)
            &&state.seenAnalogue==Address(state.analogue),"callback receives native component, vehicle and analogue identity");
        Check(std::memcmp(&state.seenAxis,&axis,4)==0&&std::memcmp(&state.seenMaximum,&maximum,4)==0,
            "callback receives the actual native axis and speed-dependent maximum without conversion");
        Check(state.accepted==unsigned(result!=0),"full integer callback result chooses exactly the correct continuation");
        for(unsigned r=0;r<8;++r) Check(state.registers[r]==(r==4?state.beforeSp-4:expected[r]),"all GP registers and seam stack preserved");
        Check(state.beforeSp==state.afterSp,"stack remains balanced across callback and continuation");
        Check(state.beforeFlags==state.afterFlags,"native flags survive destructive callback and arithmetic");
        Check(std::memcmp(state.expected.bytes,state.after.bytes,32)==0,"x87 header and MXCSR match intended native arithmetic");
        for(unsigned r=0;r<8;++r) {
            Check(std::memcmp(state.expected.bytes+32+16*r,state.after.bytes+32+16*r,10)==0,"occupied x87 stack survives callback");
            Check(std::memcmp(state.expected.bytes+160+16*r,state.after.bytes+160+16*r,16)==0,"all XMM lanes match only the intended change");
        }
        float saved=0;std::memcpy(&saved,&state.component[7],4);
        if(result) {
            const float value=axis*maximum;
            Check(std::fabs(state.output-value)<=1e-6f,"owned steering is proportional through the native computed maximum");
            Check(std::fabs(state.output)<=maximum+1e-6f,"owned steering never exceeds the native speed-dependent maximum");
            Check(std::memcmp(&saved,&state.output,4)==0,"native output and timed state are synchronized");
            Check(axis!=0.f || state.output==0.f,"neutral input immediately returns neutral steering");
        } else Check(saved==initial&&state.output==123.f,"unowned steering reaches native branch with stored state untouched");
        for(unsigned i=0;i<16;++i) if(i!=7) Check(state.component[i]==0,"steering configuration remains unchanged");
        Check(std::memcmp(&state.analogue[2],&axis,4)==0,"native analogue input memory is unchanged");
    }
    Check(VirtualFree(memory,0,MEM_RELEASE)!=0,"harness allocation released");
}
int main() {
    using fc2vr::motion::SteeringResponseInputValid;
    for(float axis:{-1.f,-.7f,-.5f,0.f,.5f,.7f,1.f}) {
        Check(SteeringResponseInputValid(axis,axis,.4f),"valid native axis and maximum can pass ownership math checks");
        Check(!SteeringResponseInputValid(axis,-axis,.4f)||axis==0.f,"opposite emitted input cannot claim the native sample");
        for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),1.1f,-1.1f}) {
            Check(!SteeringResponseInputValid(bad,axis,.4f),"invalid native axis is rejected");
            Check(!SteeringResponseInputValid(axis,bad,.4f),"invalid ownership axis is rejected");
        }
        for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),0.f,-.1f,3.2f})
            Check(!SteeringResponseInputValid(axis,axis,bad),"invalid native maximum is rejected");
    }
    Check(SteeringResponseInputValid(.5f,.50005f,.1f),"tiny native normalization difference is tolerated");
    Check(!SteeringResponseInputValid(.5f,.5002f,.1f),"different emitted/native samples are rejected");
    for(unsigned padding=0;padding<4;++padding) for(unsigned result:{0u,1u,0x100u})
        for(float axis:{-1.f,-.96f,-.7f,-.5f,-.1f,0.f,.1f,.5f,.7f,.96f,1.f})
            for(float maximum:{.125f,.4f,.75f}) Exercise(padding,result,axis,maximum);
    std::printf("native steering response: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
