#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace fc2vr::motion {
struct SteeringResponseStub { std::array<std::uint8_t,128> bytes{};std::size_t size=0; };
// Check the actual native register values against the currently owned emitted
// axis. Caller separately proves vehicle/component/status identity and liveness.
inline bool SteeringResponseInputValid(float axis,float expectedAxis,float maximum) {
    return std::isfinite(axis)&&std::isfinite(expectedAxis)&&std::isfinite(maximum)
        &&std::fabs(axis)<=1.f&&std::fabs(expectedAxis)<=1.f
        &&std::fabs(axis-expectedAxis)<=.0001f&&maximum>0.f&&maximum<=3.14159265358979323846f;
}

// GOG 0x104B9DAE: the native timed-steering calculation has already produced
// its speed-dependent maximum angle in XMM2 and current analogue axis in XMM3.
// int cdecl(component=ESI,vehicle=EDI,analogue=[EBP+0x10],float axis,float maximum) must independently
// prove current local motion ownership; this emitter does not infer ownership.
// Preserve all GP registers, flags, x87/MXCSR and XMM state around the callback.
// Acceptance changes only XMM0.low to axis*maximum, then enters native writeback
// (0x104B9E49), which updates both the stored timed angle and the output angle.
// Rejection replays the exact displaced MOVAPS/ANDPS and resumes at 0x104B9DB4.
// The native maximum/high-speed calculations and all other vehicle inputs stay
// authoritative. This is proportional position response, not a config toggle.
inline SteeringResponseStub MakeSteeringResponseStub(std::uint32_t address,std::uint32_t callback,
    std::uint32_t nativeContinuation,std::uint32_t acceptedContinuation) {
    SteeringResponseStub out;
    auto emit=[&](std::initializer_list<std::uint8_t> bytes) { for(auto b:bytes) out.bytes[out.size++]=b; };
    auto rel=[&](std::uint8_t opcode,std::uint32_t target) {
        emit({opcode});const auto delta=target-(address+std::uint32_t(out.size)+4);
        for(unsigned i=0;i<4;++i) out.bytes[out.size++]=std::uint8_t(delta>>(8*i));
    };
    emit({0x9c,0x60,0x89,0xe3}); // pushfd; pushad; mov ebx,esp
    emit({0x81,0xec,0x10,0x02,0x00,0x00,0x83,0xe4,0xf0}); // reserve528; align16
    emit({0x0f,0xae,0x04,0x24,0x83,0xec,0x08}); // fxsave; reserve float arguments
    emit({0xf3,0x0f,0x11,0x1c,0x24,0xf3,0x0f,0x11,0x54,0x24,0x04}); // axis,maximum
    emit({0xff,0x75,0x10,0x57,0x56}); // push analogue,vehicle,component
    rel(0xe8,callback);
    emit({0x83,0xc4,0x14,0x85,0xc0,0x0f,0xae,0x0c,0x24}); // discard five args; test EAX; fxrstor
    emit({0x74,0x11}); // rejected: skip accepted restore, arithmetic and jump
    emit({0x89,0xdc,0x61,0x9d}); // mov esp,ebx; popad; popfd
    emit({0xf3,0x0f,0x10,0xc3,0xf3,0x0f,0x59,0xc2}); // movss xmm0,xmm3; mulss xmm0,xmm2
    rel(0xe9,acceptedContinuation);
    emit({0x89,0xdc,0x61,0x9d});
    emit({0x0f,0x28,0xe3,0x0f,0x54,0xe5}); // displaced movaps xmm4,xmm3; andps xmm4,xmm5
    rel(0xe9,nativeContinuation);
    return out;
}
} // namespace fc2vr::motion
