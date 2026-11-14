#include "common/native_axis_gate.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// Execute the production stub in a small independent x86 caller/callback/
// continuation harness. The callback intentionally destroys caller-volatile
// registers and every FP/SIMD register. No game binary or game process is used.
static_assert(sizeof(void*)==4,"native gate execution requires Win32 x86");
static int checks=0,failures=0;
static void Check(bool ok,const char* message) {
    ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",message); }
}
static uint32_t Address(const void* p) { return uint32_t(reinterpret_cast<uintptr_t>(p)); }
struct alignas(16) Fx { unsigned char bytes[512]{}; };
struct Driver { uint32_t unused[2]{};float threshold=.35f; };
struct State {
    Fx host,before,after;
    alignas(16) uint32_t patterns[8][4]{};
    Driver driver;
    uint32_t registers[8]{},beforeSp=0,afterSp=0,beforeFlags=0,afterFlags=0;
    uint32_t stolenEsi=0,seenDriver=0,seenAxis=0,calls=0;
    uint32_t inputMxcsr=0x3f80,callbackMxcsr=0x5f80;
    uint16_t inputControl=0x077f,callbackControl=0x0b7f;
    float filtered=0;
    uint32_t alignmentPadding=0;
};
struct Code {
    uint32_t base;std::vector<uint8_t> bytes;
    explicit Code(void* address):base(Address(address)) {}
    void Emit(std::initializer_list<uint8_t> values) { bytes.insert(bytes.end(),values); }
    void Word(uint32_t value) { for(unsigned i=0;i<4;++i) bytes.push_back(uint8_t(value>>(8*i))); }
    void Absolute(std::initializer_list<uint8_t> op,const void* address) { Emit(op);Word(Address(address)); }
    void Immediate(unsigned reg,uint32_t value) { Emit({uint8_t(0xb8+reg)});Word(value); }
    void Store(unsigned reg,const void* address) { Absolute({0x89,uint8_t(0x05+8*reg)},address); }
    void Branch(uint8_t op,uint32_t target) { Emit({op});Word(target-(base+uint32_t(bytes.size())+4)); }
};
static void Write(void* address,const Code& code) { std::memcpy(address,code.bytes.data(),code.bytes.size()); }
static void Exercise(unsigned padding,unsigned result,unsigned axis,float input) {
    State state;
    for(unsigned r=0;r<8;++r) for(unsigned lane=0;lane<4;++lane)
        state.patterns[r][lane]=0x10203040u+r*0x01010101u+lane*0x11111111u;
    std::memcpy(&state.patterns[0][0],&input,4);
    const float absolute=std::fabs(input);std::memcpy(&state.patterns[1][0],&absolute,4);
    auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(memory!=nullptr,"executable harness allocation");if(!memory) return;
    auto* stub=memory+1024;auto* callback=memory+1536;auto* continuation=memory+2048;

    Code call(callback);
    call.Emit({0x8b,0x44,0x24,0x04});call.Store(0,&state.seenDriver);
    call.Emit({0x8b,0x54,0x24,0x08});call.Store(2,&state.seenAxis);
    call.Absolute({0xff,0x05},&state.calls);
    call.Emit({0xdb,0xe3,0xd9,0xee}); // fninit; fldz
    call.Absolute({0xd9,0x2d},&state.callbackControl);
    call.Absolute({0x0f,0xae,0x15},&state.callbackMxcsr);
    for(unsigned r=0;r<8;++r) call.Emit({0x66,0x0f,0x76,uint8_t(0xc0+9*r)}); // pcmpeqd xmmN,xmmN
    call.Immediate(1,0xdead1234);call.Immediate(2,0xbaad5678);
    call.Immediate(0,result);call.Emit({0x09,0xc0,0xc3}); // or eax,eax; ret
    Write(callback,call);

    Code resume(continuation);
    for(unsigned r=0;r<8;++r) resume.Store(r,&state.registers[r]);
    resume.Emit({0x9c});resume.Absolute({0x8f,0x05},&state.afterFlags);
    resume.Absolute({0x0f,0xae,0x05},&state.after);
    // Execute the original comparison direction: suppress only when threshold
    // exceeds abs(input). Captures above precede this continuation's changes.
    resume.Emit({0x0f,0x2f,0xd1,0x76,0x03,0x0f,0x57,0xc0});
    resume.Absolute({0xf3,0x0f,0x11,0x05},&state.filtered);
    resume.Absolute({0x8f,0x05},&state.stolenEsi); // consume the replayed push esi
    resume.Emit({0xc3});Write(continuation,resume);

    const auto generated=fc2vr::input::MakeAxisGateStub(Address(stub),Address(callback),Address(continuation));
    Check(generated.size<=generated.bytes.size(),"production stub stays in allocation");
    std::memcpy(stub,generated.bytes.data(),generated.size);
    Code run(memory);
    run.Emit({0x9c,0x60});run.Absolute({0x0f,0xae,0x05},&state.host);
    for(unsigned n=0;n<padding;++n) { run.Emit({0x68});run.Word(0xfeed0000+n); }
    run.Emit({0xdb,0xe3,0xd9,0xe8,0xd9,0xeb,0xd9,0xed,0xd9,0xee}); // four nonempty x87 registers
    run.Absolute({0xd9,0x2d},&state.inputControl);
    run.Absolute({0x0f,0xae,0x15},&state.inputMxcsr);
    for(unsigned r=0;r<8;++r) run.Absolute({0xf3,0x0f,0x6f,uint8_t(0x05+8*r)},&state.patterns[r]);
    const std::array<uint32_t,8> expected={0x11223344,Address(&state.driver),axis,0x55667788,0,0x12345678,0x89abcdef,0xfedcba98};
    for(unsigned r=0;r<8;++r) if(r!=4) run.Immediate(r,expected[r]);
    run.Emit({0x68});run.Word(0x246u|(padding&1?0x881u:0x14u));run.Emit({0x9d});
    run.Store(4,&state.beforeSp);
    run.Emit({0x9c});run.Absolute({0x8f,0x05},&state.beforeFlags);
    run.Absolute({0x0f,0xae,0x05},&state.before);
    run.Branch(0xe8,Address(stub));
    run.Store(4,&state.afterSp);
    run.Absolute({0x0f,0xae,0x0d},&state.host);
    if(padding) run.Emit({0x83,0xc4,uint8_t(4*padding)});
    run.Emit({0x61,0x9d,0xc3});Write(memory,run);
    DWORD prior=0;
    const bool protectedCode=VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&prior)!=0;
    Check(protectedCode,"harness code becomes executable and read-only");
    if(protectedCode) {
        FlushInstructionCache(GetCurrentProcess(),memory,4096);
        reinterpret_cast<void(*)()>(memory)();
        Check(state.calls==1,"callback executes exactly once");
        Check(state.seenDriver==Address(&state.driver)&&state.seenAxis==axis,"cdecl callback receives exact ECX driver and EDX axis");
        for(unsigned r=0;r<8;++r)
            Check(state.registers[r]==(r==4?state.beforeSp-8:expected[r]),"all general registers and seam stack preserved");
        Check(state.stolenEsi==expected[6],"displaced push esi executes exactly once");
        Check(state.beforeSp==state.afterSp,"callback and continuation leave balanced stack");
        Check(state.beforeFlags==state.afterFlags,"all saved flags survive callback");
        Check(state.driver.threshold==.35f,"driver threshold memory remains unchanged");
        const float filtered=result || absolute>=state.driver.threshold?input:0.f;
        Check(std::memcmp(&filtered,&state.filtered,4)==0,"native comparison changes only for authorized threshold bypass");
        Fx expectedFx=state.before;
        std::memset(expectedFx.bytes+160+2*16,0,16); // memory-form MOVSS clears upper lanes
        if(!result) std::memcpy(expectedFx.bytes+160+2*16,&state.driver.threshold,4);
        Check(std::memcmp(expectedFx.bytes,state.after.bytes,32)==0,"x87 header/control/status and MXCSR preserved");
        for(unsigned r=0;r<8;++r) {
            Check(std::memcmp(expectedFx.bytes+32+16*r,state.after.bytes+32+16*r,10)==0,"x87 value stack survives destructive callback");
            Check(std::memcmp(expectedFx.bytes+160+16*r,state.after.bytes+160+16*r,16)==0,"all XMM lanes preserved except intended XMM2 threshold");
        }
    }
    Check(VirtualFree(memory,0,MEM_RELEASE)!=0,"harness allocation released");
}
int main() {
    for(unsigned axis=0;axis<9;++axis) for(unsigned mask=0;mask<8;++mask)
        for(bool packet:{false,true}) for(bool current:{false,true}) {
            const bool expected=current&&packet&&((axis==4&&(mask&1))||(axis==5&&(mask&2)));
            Check(fc2vr::input::MotionAxisOwned(axis,mask,packet,current)==expected,"only current exact-packet owned left axes bypass");
        }
    Check(!fc2vr::input::MotionAxisOwned(std::numeric_limits<unsigned>::max(),~0u,true,true),"invalid axis cannot shift or bypass");
    for(unsigned padding=0;padding<4;++padding) for(unsigned result:{0u,1u,0x100u})
        for(float value:{0.f,.1f,-.1f,.35f,-.35f,.8f,-.8f,.96f,1.f,-1.f})
            Exercise(padding,result,padding&1?5u:4u,value);
    std::printf("native axis gate: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
