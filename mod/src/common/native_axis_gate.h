#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <initializer_list>

namespace fc2vr::input {
// GOG pad gate at 0x102c786d. Call int cdecl(driver=ECX,axis=EDX),
// preserving flags, all general registers, x87/MXCSR and all XMM registers.
// Only the loaded threshold XMM2 becomes zero when the callback authorizes it.
// Replay the displaced push esi and resume the original comparison/event path.
struct AxisGateStub { std::array<uint8_t,96> bytes{};size_t size=0; };
inline AxisGateStub MakeAxisGateStub(uint32_t address,uint32_t callback,uint32_t continuation) {
    AxisGateStub out;
    auto emit=[&](std::initializer_list<uint8_t> bytes) { for(auto b:bytes) out.bytes[out.size++]=b; };
    auto rel=[&](uint8_t opcode,uint32_t target) {
        emit({opcode});const uint32_t delta=target-(address+uint32_t(out.size)+4);
        for(unsigned i=0;i<4;++i) out.bytes[out.size++]=uint8_t(delta>>(i*8));
    };
    emit({0xf3,0x0f,0x10,0x51,0x08}); // movss xmm2,[ecx+8]
    emit({0x9c,0x60,0x89,0xe3}); // pushfd; pushad; mov ebx,esp
    emit({0x81,0xec,0x10,0x02,0x00,0x00,0x83,0xe4,0xf0}); // 528 bytes, align16
    emit({0x0f,0xae,0x04,0x24,0x52,0x51}); // fxsave [esp]; push edx; push ecx
    rel(0xe8,callback);
    emit({0x83,0xc4,0x08,0x85,0xc0}); // discard args; test eax,eax
    emit({0x0f,0xae,0x0c,0x24}); // fxrstor [esp], preserves integer flags
    emit({0x74,0x03,0x0f,0x57,0xd2}); // jz +3; xorps xmm2,xmm2
    emit({0x89,0xdc,0x61,0x9d,0x56}); // mov esp,ebx; popad; popfd; push esi
    rel(0xe9,continuation);
    return out;
}
// Ownership is derived from the exact returned synthetic pad packet. Axis 4/5
// are left X/Y; no triggers, right stick or native pad axis can opt in.
inline bool MotionAxisOwned(unsigned axis,unsigned mask,bool packetMatches,bool current) {
    return current && packetMatches && axis>=4 && axis<=5 && (mask&(1u<<(axis-4)))!=0;
}
} // namespace fc2vr::input
