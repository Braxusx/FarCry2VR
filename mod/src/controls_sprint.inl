// Included inside namespace controls. GOG 1.03 bounded native movement adapter.
// GOG instruction windows are guarded before installation; see OPTIONAL_MOTION.md.
static int __cdecl NativeSprintDirection(void* controller) {
    if(!controller || IsBadReadPtr(controller,0x24)) return false;
    const auto p=static_cast<const uint8_t*>(controller);
    const bool local=arms::IsLocalPawn(*(void* const*)(p+0x20));
    if(!local || !MotionSprintCurrent()) return false;
    const float forward=*(const float*)(p+8),side=*(const float*)(p+0xc);
    const bool locked=p[0x18]!=0;
    const bool allowed=fc2vr::motion::SprintDirectionOwned(true,local,locked,forward,side);
    static DWORD lastReport=0;static unsigned reports=0;
    const DWORD now=GetTickCount();
    if(g_c.motionInputTrace && reports<60 && (reports==0 || now-lastReport>2000)) {
        ++reports;lastReport=now;
        LOG("motion-27 native sprint: direction=%d local=%d locked=%d forward=%.3f side=%.3f",allowed,local,locked,forward,side);
    }
    return allowed;
}
static bool InstallMotionSprintGate(HMODULE dunia,PatchFn patch) {
    if(!g_c.armSwingSprint || gameaddr::Detect()!=gameaddr::GOG) return false;
    const auto base=reinterpret_cast<uintptr_t>(dunia);
    auto va=[&](uintptr_t address) { return base+address-0x10000000; };
    uint8_t expected[]={
        0xf3,0x0f,0x10,0x46,0x0c,0x8b,0x4e,0x20,0xf3,0x0f,0x11,0x44,0x24,0x2c,
        0xe8,0xc9,0xa2,0xf3,0xff,0xf3,0x0f,0x10,0x0d,0x10,0x60,0xd8,0x10,
        0xf3,0x0f,0x10,0x44,0x24,0x2c,0x0f,0x54,0xc1,0xf3,0x0f,0x10,0x88,
        0xa8,0x00,0x00,0x00,0x0f,0x2f,0xc8,0x72,0x0e,0xf3,0x0f,0x10,0x46,
        0x08,0x0f,0x2f,0x05,0x9c,0x38,0xd8,0x10,0x77,0x20};
    const uint32_t absMask=uint32_t(va(0x10d86010)),zero=uint32_t(va(0x10d8389c));
    memcpy(expected+23,&absMask,4);memcpy(expected+57,&zero,4);
    auto site=reinterpret_cast<uint8_t*>(va(0x10143604));
    auto facing=reinterpret_cast<uint8_t*>(va(0x101436dd));
    uint8_t expectedFacing[]={0x0f,0x2f,0x05,0x9c,0x38,0xd8,0x10,0x0f,0x82,0x59,0xff,0xff,0xff};
    memcpy(expectedFacing+3,&zero,4);
    static const uint8_t next[]={0xf6,0x05}; // next native sprint eligibility block
    if(memcmp(site,expected,sizeof expected) || memcmp(facing,expectedFacing,sizeof expectedFacing)
        || memcmp((void*)va(0x10143663),next,sizeof next)) {
        LOG("motion-27 native sprint: directional gate byte guard mismatch; unchanged");return false;
    }
    auto memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,256,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!memory) return false;
    const auto stub=fc2vr::motion::MakeSprintGateStub(uint32_t(uintptr_t(memory)),
        uint32_t(uintptr_t(&NativeSprintDirection)),uint32_t(va(0x1014360c)),uint32_t(va(0x10143663)));
    memcpy(memory,stub.bytes.data(),stub.size);
    const auto facingStub=fc2vr::motion::MakeSprintFacingStub(uint32_t(uintptr_t(memory+128)),
        uint32_t(uintptr_t(&NativeSprintDirection)),zero,uint32_t(va(0x10143643)),uint32_t(va(0x101436ea)));
    memcpy(memory+128,facingStub.bytes.data(),facingStub.size);
    DWORD old=0;
    if(!VirtualProtect(memory,256,PAGE_EXECUTE_READ,&old)) { VirtualFree(memory,0,MEM_RELEASE);return false; }
    FlushInstructionCache(GetCurrentProcess(),memory,256);
    static void* original=nullptr;static void* originalFacing=nullptr;
    if(!patch(uintptr_t(site),expected,8,memory,&original)) { VirtualFree(memory,0,MEM_RELEASE);return false; }
    if(!patch(uintptr_t(facing),expectedFacing,sizeof expectedFacing,memory+128,&originalFacing)) {
        // Both guards were checked before either write. Still fail safely if
        // another patch changed the second site between validation and commit.
        if(VirtualProtect(site,8,PAGE_EXECUTE_READWRITE,&old)) {
            memcpy(site,expected,8);DWORD ignored=0;VirtualProtect(site,8,old,&ignored);
            FlushInstructionCache(GetCurrentProcess(),site,8);VirtualFree(memory,0,MEM_RELEASE);
            LOG("motion-27 native sprint: second gate failed; first gate restored");
        } else LOG("motion-27 native sprint: second gate failed; first remains scoped (restore protection failed)");
        return false;
    }
    LOG("motion-27 native sprint: both scoped local-pawn directional gates installed");return true;
}
