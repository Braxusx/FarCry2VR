// Included in controls after NGA and NativeSteeringOwner. Resolve only on the
// native input/game thread, while a current local driver motion lease is held.
// Borrowed identities only; the physics callback must not run these queries.
static bool g_nativeSteeringOwnerReady=false;
static bool NativeOwnerReadable(uintptr_t p,size_t bytes) {
    return p && bytes && p<=UINTPTR_MAX-bytes && !IsBadReadPtr((const void*)p,bytes);
}
static bool NativeOwnerCode(uintptr_t va,const uint8_t* expected,size_t bytes) {
    const uintptr_t p=NGA(va);
    return NativeOwnerReadable(p,bytes) && !memcmp((const void*)p,expected,bytes);
}
static bool InitNativeSteeringOwner() {
    // Relocation-independent instruction windows: no embedded absolute VAs.
    static const uint8_t getter[]={0x83,0xec,0x08,0x8b,0x4c,0x24,0x0c,0x8b,0x01,0x8b,0x50,0x7c,0x53};
    static const uint8_t release[]={0x01,0x5e,0x08,0x8b,0xf8,0x75,0x10};
    static const uint8_t query[]={0x56,0x8b,0xf1,0xf7,0x46,0x70,0x00,0x10,0x00,0x00};
    static const uint8_t wrapper[]={0x56,0x8b,0xf1,0x83,0x7e,0x64,0x00,0x74,0x18,0x8b,0x46,0x60,0x8b,0x08,0x8b,0x11,0x8b,0x42,0x08,0xff,0xd0,0x83,0xf8,0x07,0x75,0x07,0x8b,0x4e,0x60,0x8b,0x01,0x5e,0xc3,0x33,0xc0,0x5e,0xc3};
    static const uint8_t driver[]={0x8b,0x44,0x24,0x08,0x8b,0x49,0x10,0x56,0x8b,0x74,0x24,0x08,0x50,0x56,0xe8,0x8d,0xf7,0xff,0xff,0x8b,0xc6,0x5e,0xc2,0x08,0x00};
    static const uint8_t seat[]={0x8b,0x51,0x10,0x83,0xec,0x08,0x33,0xc0,0x85,0xd2,0x56,0x76,0x15,0x8b,0x71,0x0c,0x8d,0x4e,0x10,0x83,0x39,0x01,0x74,0x1d,0x83,0xc0,0x01,0x83,0xc1,0x50,0x3b,0xc2,0x72,0xf1};
    static const uint8_t baseLink[]={0x89,0x7e,0x54};
    static const uint8_t wrapperLink[]={0x89,0x7e,0x5c,0xc6,0x46,0x3c,0x01,0xe8,0x53,0x91,0x01,0x00};
    static const uint8_t implLink[]={0x89,0xbe,0x80,0x01,0x00,0x00};
    g_nativeSteeringOwnerReady=g_base
        && NativeOwnerCode(0x100E6160,getter,sizeof getter)
        && NativeOwnerCode(0x100E621F,release,sizeof release)
        && NativeOwnerCode(0x104D5530,query,sizeof query)
        && NativeOwnerCode(0x10068D10,wrapper,sizeof wrapper)
        && NativeOwnerCode(0x100D7910,driver,sizeof driver)
        && NativeOwnerCode(0x100D70B0,seat,sizeof seat)
        && NativeOwnerCode(0x1048529D,baseLink,sizeof baseLink)
        && NativeOwnerCode(0x10485901,wrapperLink,sizeof wrapperLink)
        && NativeOwnerCode(0x1049EA78,implLink,sizeof implLink);
    return g_nativeSteeringOwnerReady;
}
static NativeSteeringOwner ResolveSteeringOwner(void* pawn) {
    NativeSteeringOwner out{};
    const uintptr_t p=(uintptr_t)pawn;
    if (!g_nativeSteeringOwnerReady || !NativeOwnerReadable(p,0x14)) return out;
    const uintptr_t pawnVtable=*(uintptr_t*)p;
    if (!NativeOwnerReadable(pawnVtable,0x80)
        || IsBadCodePtr((FARPROC)*(uintptr_t*)(pawnVtable+0x7C))) return out;
    const uintptr_t pawnHandle=*(uintptr_t*)(p+8);
    if (!NativeOwnerReadable(pawnHandle,0x10)) return out;
    const uintptr_t pawnEntity=*(uintptr_t*)(pawnHandle+0xC);
    if (!NativeOwnerReadable(pawnEntity,0xA0)) return out;
    const uint32_t idLo=*(uint32_t*)pawnHandle,idHi=*(uint32_t*)(pawnHandle+4);
    if ((idLo==0xFFFFFFFFu && idHi==0xFFFFFFFFu)
        || *(uint32_t*)(pawnEntity+8)!=idLo || *(uint32_t*)(pawnEntity+0xC)!=idHi) return out;
    // Both native type registries must already exist. Do not bootstrap them.
    const uintptr_t vehicleType=NGA(0x10F2F07C),wheeledType=NGA(0x10F2D960),descriptor=NGA(0x10F2D978);
    if (!NativeOwnerReadable(vehicleType,0x18) || !*(uintptr_t*)vehicleType
        || !NativeOwnerReadable(wheeledType,0x1C) || !*(uintptr_t*)wheeledType
        || !NativeOwnerReadable(descriptor,4)) return out;
    typedef uintptr_t (__cdecl *CurrentVehicleFn)(void*);
    const uintptr_t vehicle=((CurrentVehicleFn)NGA(0x100E6160))(pawn);
    if (!NativeOwnerReadable(vehicle,0x1C) || !NativeOwnerReadable(*(uintptr_t*)vehicle,4)) return out;
    const uintptr_t data=*(uintptr_t*)(vehicle+0x10);
    if (!NativeOwnerReadable(data,0x14)) return out;
    const uintptr_t seats=*(uintptr_t*)(data+0xC);
    const uint32_t count=*(uint32_t*)(data+0x10);
    if (!count || count>32 || !NativeOwnerReadable(seats,count*0x50)) return out;
    typedef uint32_t* (__fastcall *DriverFn)(void*,void*,uint32_t*,int);
    uint32_t driverId[2]={0xFFFFFFFFu,0xFFFFFFFFu};
    ((DriverFn)NGA(0x100D7910))((void*)vehicle,nullptr,driverId,0);
    if (driverId[0]!=idLo || driverId[1]!=idHi) return out;
    const uintptr_t vehicleHandle=*(uintptr_t*)(vehicle+8);
    if (!NativeOwnerReadable(vehicleHandle,0x10)) return out;
    const uintptr_t entity=*(uintptr_t*)(vehicleHandle+0xC);
    if (!NativeOwnerReadable(entity,0xA0) || !NativeOwnerReadable(*(uintptr_t*)entity,4)) return out;
    if (*(uint32_t*)(entity+8)!=*(uint32_t*)vehicleHandle
        || *(uint32_t*)(entity+0xC)!=*(uint32_t*)(vehicleHandle+4)) return out;
    typedef uintptr_t (__fastcall *QueryFn)(void*,void*,const void*);
    const uintptr_t comp=((QueryFn)NGA(0x104D5530))((void*)entity,nullptr,(void*)descriptor);
    if (!NativeOwnerReadable(comp,0x68) || !NativeOwnerReadable(*(uintptr_t*)comp,4)) return out;
    const uint32_t wrapperCount=*(uint32_t*)(comp+0x64);
    const uintptr_t wrappers=*(uintptr_t*)(comp+0x60);
    if (!wrapperCount || wrapperCount>64 || !NativeOwnerReadable(wrappers,wrapperCount*4)) return out;
    const uintptr_t wrapper=*(uintptr_t*)wrappers;
    if (!NativeOwnerReadable(wrapper,0x60) || *(uintptr_t*)wrapper!=NGA(0x10DDD6C0)
        || !NativeOwnerReadable(*(uintptr_t*)wrapper,0x138)) return out;
    typedef uintptr_t (__fastcall *WrapperFn)(void*,void*);
    if (((WrapperFn)NGA(0x10068D10))((void*)comp,nullptr)!=wrapper) return out;
    const uintptr_t physics=*(uintptr_t*)(wrapper+0x5C);
    if (!NativeOwnerReadable(physics,0x188) || *(uintptr_t*)physics!=NGA(0x10DDF088)
        || !NativeOwnerReadable(*(uintptr_t*)physics,0x140)
        || *(uintptr_t*)(wrapper+0x54)!=physics || *(uintptr_t*)(physics+0x180)!=wrapper) return out;
    const uintptr_t havok=*(uintptr_t*)(physics+0x184);
    if (!NativeOwnerReadable(havok,0xD8) || !NativeOwnerReadable(*(uintptr_t*)havok,4)
        || *(uintptr_t*)(havok+0xD4)!=physics) return out;
    const uintptr_t component=*(uintptr_t*)(havok+0x24),analog=*(uintptr_t*)(havok+0x9C);
    if (!NativeOwnerReadable(component,0x40) || !NativeOwnerReadable(analog,0x10)) return out;
    const uintptr_t steeringVtable=*(uintptr_t*)component;
    if (!NativeOwnerReadable(steeringVtable,0x14)
        || *(uintptr_t*)(steeringVtable+0x10)!=NGA(0x104B9C80)) return out;
    out.pawn=p;out.vehicle=vehicle;out.physics=physics;out.havok=havok;
    out.component=component;out.analog=analog;return out;
}
