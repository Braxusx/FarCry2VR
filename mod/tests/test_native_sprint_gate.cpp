#include "common/native_sprint_gate.h"
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
struct Driver { uint32_t unused[3]{};float side=.35f;uint32_t middle[4]{};uint32_t owner=0x456789ab; };
struct State {
    Fx host,before,after;
    alignas(16) uint32_t patterns[8][4]{};
    Driver driver;
    uint32_t registers[8]{},beforeSp=0,afterSp=0,beforeFlags=0,afterFlags=0;
    uint32_t accepted=0,seenDriver=0,seenAxis=0,calls=0;
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
static void Exercise(unsigned padding,unsigned result,unsigned axis,float input,bool facing) {
    State state;
    for(unsigned r=0;r<8;++r) for(unsigned lane=0;lane<4;++lane)
        state.patterns[r][lane]=0x10203040u+r*0x01010101u+lane*0x11111111u;
    std::memcpy(&state.patterns[0][0],&input,4);
    const float absolute=std::fabs(input);std::memcpy(&state.patterns[1][0],&absolute,4);
    auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(memory!=nullptr,"executable harness allocation");if(!memory) return;
    auto* stub=memory+1024;auto* callback=memory+1536;auto* continuation=memory+2048;auto* accepted=memory+2304;

    Code call(callback);
    call.Emit({0x8b,0x44,0x24,0x04});call.Store(0,&state.seenDriver);
    call.Absolute({0xff,0x05},&state.calls);
    call.Emit({0xdb,0xe3,0xd9,0xee}); // fninit; fldz
    call.Absolute({0xd9,0x2d},&state.callbackControl);
    call.Absolute({0x0f,0xae,0x15},&state.callbackMxcsr);
    for(unsigned r=0;r<8;++r) call.Emit({0x66,0x0f,0x76,uint8_t(0xc0+9*r)}); // pcmpeqd xmmN,xmmN
    call.Immediate(1,0xdead1234);call.Immediate(2,0xbaad5678);
    call.Immediate(0,result);call.Emit({0x09,0xc0,0xc3}); // or eax,eax; ret
    Write(callback,call);

    for(bool bypass:{false,true}) {
        Code resume(bypass?accepted:continuation);
        for(unsigned r=0;r<8;++r) resume.Store(r,&state.registers[r]);
        resume.Emit({0x9c});resume.Absolute({0x8f,0x05},&state.afterFlags);
        resume.Absolute({0x0f,0xae,0x05},&state.after);
        resume.Absolute({0xc7,0x05},&state.accepted);resume.Word(bypass?1u:0u);
        resume.Emit({0xc3});Write(bypass?accepted:continuation,resume);
    }
    const float zero=0.f;
    const auto generated=facing
        ? fc2vr::motion::MakeSprintFacingStub(Address(stub),Address(callback),Address(&zero),Address(continuation),Address(accepted))
        : fc2vr::motion::MakeSprintGateStub(Address(stub),Address(callback),Address(continuation),Address(accepted));
    Check(generated.size<=generated.bytes.size(),"production stub stays in allocation");
    std::memcpy(stub,generated.bytes.data(),generated.size);
    Code run(memory);
    run.Emit({0x9c,0x60});run.Absolute({0x0f,0xae,0x05},&state.host);
    for(unsigned n=0;n<padding;++n) { run.Emit({0x68});run.Word(0xfeed0000+n); }
    run.Emit({0xdb,0xe3,0xd9,0xe8,0xd9,0xeb,0xd9,0xed,0xd9,0xee}); // four nonempty x87 registers
    run.Absolute({0xd9,0x2d},&state.inputControl);
    run.Absolute({0x0f,0xae,0x15},&state.inputMxcsr);
    for(unsigned r=0;r<8;++r) run.Absolute({0xf3,0x0f,0x6f,uint8_t(0x05+8*r)},&state.patterns[r]);
    const std::array<uint32_t,8> expected={0x11223344,0x567123ab,axis,0x55667788,0,0x12345678,Address(&state.driver),0xfedcba98};
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
        Check(state.seenDriver==Address(&state.driver),"cdecl callback receives exact ESI controller");
        for(unsigned r=0;r<8;++r)
            Check(state.registers[r]==(r==4?state.beforeSp-4:(r==1&&!result&&!facing?state.driver.owner:expected[r])),"GP registers preserved except original ECX load on native route");
        Check(state.accepted==((result||(facing&&input>=0))?1u:0u),"callback or original COMISS chooses correct native/VR continuation");
        Check(state.beforeSp==state.afterSp,"callback and continuation leave balanced stack");
        const uint32_t expectedFlags=facing&&!result
            ? (state.beforeFlags&~0x8d5u)|(input<0?1u:(input==0?0x40u:0u)) : state.beforeFlags;
        Check(expectedFlags==state.afterFlags,"saved flags survive callback or match original COMISS");
        Check(state.driver.side==.35f && state.driver.owner==0x456789ab,"controller memory unchanged");
        Fx expectedFx=state.before;
        if(!result&&!facing) {
            std::memset(expectedFx.bytes+160,0,16); // original memory MOVSS clears upper lanes
            std::memcpy(expectedFx.bytes+160,&state.driver.side,4);
        }
        Check(std::memcmp(expectedFx.bytes,state.after.bytes,32)==0,"x87 header/control/status and MXCSR preserved");
        for(unsigned r=0;r<8;++r) {
            Check(std::memcmp(expectedFx.bytes+32+16*r,state.after.bytes+32+16*r,10)==0,"x87 value stack survives destructive callback");
            Check(std::memcmp(expectedFx.bytes+160+16*r,state.after.bytes+160+16*r,16)==0,"all XMM lanes preserved except original XMM0 load on native route");
        }
    }
    Check(VirtualFree(memory,0,MEM_RELEASE)!=0,"harness allocation released");
}
int main() {
    for(bool current:{false,true}) for(bool local:{false,true}) for(bool locked:{false,true}) {
        for(int angle=0;angle<360;angle+=5) {
            const float theta=angle*3.14159265f/180;
            Check(fc2vr::motion::SprintDirectionOwned(current,local,locked,std::cos(theta),std::sin(theta))
                ==(current&&local&&!locked),"only local current unlocked VR sprint owns all input headings");
        }
    }
    for(float bad:{0.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),3.f})
        Check(!fc2vr::motion::SprintDirectionOwned(true,true,false,bad,0),"invalid/zero native axes never bypass");
    for(unsigned padding=0;padding<4;++padding) for(unsigned result:{0u,1u,0x100u})
        for(float value:{0.f,.1f,-.1f,.35f,-.35f,.8f,-.8f,1.f,-1.f})
            for(bool facing:{false,true}) Exercise(padding,result,padding,value,facing);
    std::printf("native sprint gate: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
