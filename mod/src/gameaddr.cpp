#include "gameaddr.h"
#include "log.h"
#include <windows.h>
#include <array>

namespace gameaddr {
#include "gogmap.inc"
static volatile LONG g_build=-1;
bool InspectFile(const wchar_t* path,fc2vr::image::Identity& out) {
    out={};
    if(!path || !*path) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok=GetFileSizeEx(file,&size) && size.QuadPart>=64 && size.QuadPart<=0x10000000;
    std::array<std::uint8_t,65536> buffer{};
    fc2vr::image::Sha256 hash; fc2vr::image::Identity id;
    std::uint64_t total=0;
    while(ok) {
        DWORD count=0;
        if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)) { ok=false; break; }
        if(!count) break;
        if(total==0 && !fc2vr::image::ParsePe(buffer.data(),count,id.pe)) { ok=false; break; }
        if(total+count>static_cast<std::uint64_t>(size.QuadPart) || !hash.Update(buffer.data(),count)) { ok=false; break; }
        total+=count;
    }
    LARGE_INTEGER endSize{};
    ok=ok && total==static_cast<std::uint64_t>(size.QuadPart) && GetFileSizeEx(file,&endSize) && endSize.QuadPart==size.QuadPart && hash.Finish(id.sha);
    CloseHandle(file);
    if(!ok) return false;
    id.fileSize=total; id.measured=true; out=id; return true;
}
static bool Readable(const void* pointer,std::size_t size,HMODULE module) {
    auto p=reinterpret_cast<std::uintptr_t>(pointer);
    if(!p || size>UINTPTR_MAX-p) return false;
    const auto end=p+size;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi)) || mbi.AllocationBase!=module || mbi.State!=MEM_COMMIT
            || (mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        const auto base=reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if(mbi.RegionSize>UINTPTR_MAX-base || base+mbi.RegionSize<=p) return false;
        p=base+mbi.RegionSize;
    }
    return true;
}
static bool MappedMatches(HMODULE module,const fc2vr::image::Pe& file) {
    const auto* base=reinterpret_cast<const std::uint8_t*>(module);
    if(!Readable(base,64,module) || fc2vr::image::U16(base)!=0x5a4d) return false;
    const auto nt=fc2vr::image::U32(base+0x3c);
    if(nt<64 || nt>65536-24 || !Readable(base,nt+24,module)) return false;
    const auto optional=fc2vr::image::U16(base+nt+20);
    if(optional>65536-nt-24 || !Readable(base,nt+24+optional,module)) return false;
    fc2vr::image::Pe mapped;
    return fc2vr::image::ParsePe(base,nt+24+optional,mapped) && mapped==file;
}
int Detect() {
    LONG build=InterlockedCompareExchange(&g_build,-1,-1);
    if(build>=0) return build;
    // Do not wait on an initialization lock from DllMain/loader callbacks. A
    // concurrent observer gets no native authority while measurement is in flight.
    build=InterlockedCompareExchange(&g_build,-2,-1);
    if(build!=-1) return build>=0?build:Unknown;
    HMODULE module=GetModuleHandleW(L"Dunia.dll");
    if(!module) { InterlockedExchange(&g_build,-1); return Unknown; }
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(module,path,32768);
    fc2vr::image::Identity id;
    const bool measured=length>0 && length<32768 && InspectFile(path,id);
    const bool mapped=measured && MappedMatches(module,id.pe);
    build=mapped?static_cast<LONG>(fc2vr::image::Select(id)):Unknown;
    InterlockedExchange(&g_build,build);
    char digest[65]; fc2vr::image::Hex(id.sha,digest);
    LOG("image-gate-1 profile=%s accepted=%d measured=%d mappedHeader=%d bytes=%llu sha256=%s timestamp=%08x imageSize=%08x; unknown has no native addresses",
        build==Steam?"Steam exact":build==GOG?"GOG exact":"unsupported",build!=Unknown,measured,mapped,
        (unsigned long long)id.fileSize,digest,(unsigned)id.pe.timestamp,(unsigned)id.pe.imageSize);
    return build;
}
const char* Name() { return g_build==Steam?"Steam":g_build==GOG?"GOG":"unknown"; }
uint32_t Xlate(uint32_t va) {
    const int build=Detect();
    return fc2vr::image::Translate(static_cast<fc2vr::image::Build>(build),va,kGogMap,sizeof(kGogMap)/sizeof(kGogMap[0]));
}
}
