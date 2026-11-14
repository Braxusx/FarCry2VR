// Included inside namespace controls, after the guarded ownership resolver.
static int __cdecl NativeSteeringResponse(void* component,void* havok,void* analog,float axis,float maximum) {
    if(!g_steeringResponseEnabled || !g_c.steeringDirect) return false;
    AcquireSRWLockShared(&g_wheelHoldLock);
    const auto source=g_wheelHoldSource;const auto owner=g_wheelOwner;
    const uint8_t mask=g_wheelHoldMask;const bool owned=g_wheelMotionOwned;
    const float emitted=g_wheelEmittedAxis;
    ReleaseSRWLockShared(&g_wheelHoldLock);
    // Resolve only during local input polling; never discover/guess a vehicle
    // from this shared native physics callback. Other vehicles immediately fall through.
    if(!owned || !owner.component || owner.component!=(uintptr_t)component
        || owner.havok!=(uintptr_t)havok || owner.analog!=(uintptr_t)analog
        || !arms::IsLocalPawn((void*)owner.pawn)) return false;
    // Recheck the cached tuple without calling game getters from physics.
    const bool live=NativeOwnerReadable(owner.physics,0x188)
        && *(uintptr_t*)owner.physics==NGA(0x10DDF088)
        && *(uintptr_t*)(owner.physics+0x184)==owner.havok
        && NativeOwnerReadable(owner.havok,0xD8)
        && *(uintptr_t*)(owner.havok+0xD4)==owner.physics
        && *(uintptr_t*)(owner.havok+0x24)==owner.component
        && *(uintptr_t*)(owner.havok+0x9C)==owner.analog
        && NativeOwnerReadable(owner.component,0x40)
        && NativeOwnerReadable(*(uintptr_t*)owner.component,0x14)
        && *(uintptr_t*)(*(uintptr_t*)owner.component+0x10)==NGA(0x104B9C80)
        && NativeOwnerReadable(owner.analog,0x10);
    const bool current=live && CurrentWheelMask(source,mask)!=0;
    // Axis() emits SHORT/32767. Native XInput uses /32768 on the negative
    // half, then saturates at |axis|>=.96. The intervening event/vehicle path
    // preserves sign and magnitude. A different native sample fails closed.
    float expected=emitted<0 ? emitted*(32767.f/32768.f) : emitted;
    if(fabsf(expected)>=.96f) expected=expected<0 ? -1.f : 1.f;
    const bool valid=current && std::isfinite(emitted) && fabsf(emitted)<=1.f
        && fc2vr::motion::SteeringResponseInputValid(axis,expected,maximum)
        && std::isfinite(*(float*)(owner.analog+8)) && fabsf(*(float*)(owner.analog+8)-axis)<=.0001f;
    static ULONGLONG last=0;static unsigned reports=0;
    const auto now=GetTickCount64();
    if(g_c.motionInputTrace && reports<60 && (!reports || now-last>=2000)) {
        last=now;++reports;
        LOG("motion-28 native wheel: direct=%d live=%d current=%d axis=%.4f expected=%.4f maximum=%.4frad",
            valid,live,current,axis,expected,maximum);
    }
    return valid;
}
static void InstallMotionSteeringResponse(PatchFn patch) {
    if(!patch || !g_c.motionSteering || !g_c.steeringDirect || gameaddr::Detect()!=gameaddr::GOG) return;
    static const uint8_t prelude[]={0xf3,0x0f,0x10,0x56,0x2c,0xf3,0x0f,0x5c,0xd4,0xf3,0x0f,0x59,0xd3,0xf3,0x0f,0x10,0x5c,0x24,0x0c,0xf3,0x0f,0x58,0xd4};
    static const uint8_t expected[]={0x0f,0x28,0xe3,0x0f,0x54,0xe5};
    static const uint8_t writeback[]={0x8b,0x55,0x14,0xf3,0x0f,0x11,0x46,0x1c,0xd9,0x46,0x1c,0x5f,0xd9,0x1a,0x5e,0x8b,0xe5,0x5d,0xc2,0x10,0x00};
    if(!InitNativeSteeringOwner() || !NativeOwnerCode(0x104B9D97,prelude,sizeof prelude)
        || !NativeOwnerCode(0x104B9DAE,expected,sizeof expected)
        || !NativeOwnerCode(0x104B9E49,writeback,sizeof writeback)) {
        LOG("motion-28 native wheel: owner/response byte guard mismatch; timed steering retained");return;
    }
    auto memory=(uint8_t*)VirtualAlloc(nullptr,128,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!memory) return;
    const auto code=fc2vr::motion::MakeSteeringResponseStub(uint32_t(uintptr_t(memory)),
        uint32_t(uintptr_t(&NativeSteeringResponse)),uint32_t(NGA(0x104B9DB4)),uint32_t(NGA(0x104B9E49)));
    memcpy(memory,code.bytes.data(),code.size);DWORD old=0;
    if(!VirtualProtect(memory,128,PAGE_EXECUTE_READ,&old)) { VirtualFree(memory,0,MEM_RELEASE);return; }
    FlushInstructionCache(GetCurrentProcess(),memory,code.size);
    void* trampoline=nullptr;
    g_steeringResponseEnabled=patch(NGA(0x104B9DAE),expected,sizeof expected,memory,&trampoline);
    if(!g_steeringResponseEnabled) VirtualFree(memory,0,MEM_RELEASE);
    LOG("motion-28 native wheel: scoped direct response installed=%d (native speed limit and writeback retained)",g_steeringResponseEnabled);
}
