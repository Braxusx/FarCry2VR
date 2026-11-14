#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace fc2vr::motion {
// This authorizes only the native sprint direction test. Native sprint request,
// stamina, posture, animation, speed and collision checks remain authoritative.
inline bool SprintDirectionOwned(bool current,bool local,bool locked,float forward,float side) {
    const float length2=forward*forward+side*side;
    return current && local && !locked && std::isfinite(forward) && std::isfinite(side)
        && std::isfinite(length2) && length2>.01f && length2<=2.01f;
}
struct SprintGateStub { std::array<std::uint8_t,128> bytes{};std::size_t size=0; };
// GOG 0x10143604: ESI is the native movement controller. Preserve integer flags,
// all general registers, x87/MXCSR and XMM state around int cdecl(controller).
// A rejected callback replays both displaced instructions; acceptance skips
// only the native forward/strafe eligibility test, using the existing vector.
inline SprintGateStub MakeSprintGateStub(std::uint32_t address,std::uint32_t callback,
    std::uint32_t nativeContinuation,std::uint32_t acceptedContinuation) {
    SprintGateStub out;
    auto emit=[&](std::initializer_list<std::uint8_t> bytes) { for(auto b:bytes) out.bytes[out.size++]=b; };
    auto rel=[&](std::uint8_t opcode,std::uint32_t target) {
        emit({opcode});const std::uint32_t delta=target-(address+std::uint32_t(out.size)+4);
        for(unsigned i=0;i<4;++i) out.bytes[out.size++]=std::uint8_t(delta>>(i*8));
    };
    emit({0x9c,0x60,0x89,0xe3}); // pushfd; pushad; mov ebx,esp
    emit({0x81,0xec,0x10,0x02,0x00,0x00,0x83,0xe4,0xf0}); // reserve528, align16
    emit({0x0f,0xae,0x04,0x24,0x56}); // fxsave [esp]; push esi
    rel(0xe8,callback);
    emit({0x83,0xc4,0x04,0x85,0xc0,0x0f,0xae,0x0c,0x24}); // discard arg; test; fxrstor
    emit({0x74,0x09}); // rejected: skip accepted restore/jump
    emit({0x89,0xdc,0x61,0x9d});rel(0xe9,acceptedContinuation);
    emit({0x89,0xdc,0x61,0x9d}); // mov esp,ebx; popad; popfd
    emit({0xf3,0x0f,0x10,0x46,0x0c,0x8b,0x4e,0x20}); // original movss/mov
    rel(0xe9,nativeContinuation);
    return out;
}
// The second GOG direction-only test (0x101436DD) rejects negative dot product
// between the current input and native entity forward. Its fallback must replay
// COMISS and the original below branch, rather than preserving incoming flags.
inline SprintGateStub MakeSprintFacingStub(std::uint32_t address,std::uint32_t callback,
    std::uint32_t zero,std::uint32_t rejectedContinuation,std::uint32_t acceptedContinuation) {
    auto out=MakeSprintGateStub(address,callback,acceptedContinuation,acceptedContinuation);
    out.size-=13; // replace the first gate's native MOVSS/MOV/JMP replay
    auto byte=[&](std::uint8_t value) { out.bytes[out.size++]=value; };
    auto word=[&](std::uint32_t value) { for(unsigned i=0;i<4;++i) byte(std::uint8_t(value>>(i*8))); };
    for(auto value:{0x0f,0x2f,0x05}) byte(std::uint8_t(value));word(zero); // comiss xmm0,[zero]
    byte(0x0f);byte(0x82);word(rejectedContinuation-(address+std::uint32_t(out.size)+4)); // jb
    byte(0xe9);word(acceptedContinuation-(address+std::uint32_t(out.size)+4));
    return out;
}
}
