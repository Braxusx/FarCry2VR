#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>
#include <limits>
#include "../src/common/native_gog_address.h"
#include "../src/common/native_steering_response.h"
#include "../src/common/pose_authority.h"

namespace xr { using InputStamp=fc2vr::authority::Stamp; }
namespace arms { static bool IsLocalPawn(const void*) { return false; } }
namespace gameaddr {
enum Version { GOG, Steam };
static Version version=GOG;
static Version Detect() { return version; }
}
#define LOG(...) ((void)0)
namespace controls {
using PatchFn=bool (*)(uintptr_t,const uint8_t*,size_t,void*,void**);
struct NativeSteeringOwner { uintptr_t pawn=0,vehicle=0,physics=0,havok=0,component=0,analog=0; };
static struct Config { bool steeringDirect=true,motionSteering=true,motionInputTrace=false; } g_c;
static uintptr_t g_base=0;
static SRWLOCK g_wheelHoldLock=SRWLOCK_INIT;
static xr::InputStamp g_wheelHoldSource;
static NativeSteeringOwner g_wheelOwner;
static uint8_t g_wheelHoldMask=0;
static bool g_wheelMotionOwned=false,g_steeringResponseEnabled=false;
static float g_wheelEmittedAxis=0;
static unsigned translatedCalls=0;
// Deliberately refuse the ordinary Steam-to-GOG translator. If production
// accidentally routes these already-GOG VAs through it again, installation fails.
static uintptr_t GA(uintptr_t) { ++translatedCalls;return 0; }
static uintptr_t NGA(uintptr_t va) {
    return fc2vr::native::GogImageAddress(g_base,gameaddr::Detect()==gameaddr::GOG,va);
}
static uint8_t CurrentWheelMask(const xr::InputStamp&,uint8_t) { return 0; }
#include "../src/controls_vehicle_owner.inl"
#include "../src/controls_steering.inl"
}

namespace {
constexpr uintptr_t PreferredBase=0x10000000;
constexpr size_t ImageBytes=0x0192A000;
struct Window { uintptr_t va;std::vector<uint8_t> bytes; };
// Independently pinned instruction windows from the GOG PE audit. The optional
// DLL mode reads these bytes from disk instead; no DLL code is ever executed.
const Window windows[]={
    {0x100E6160,{0x83,0xec,0x08,0x8b,0x4c,0x24,0x0c,0x8b,0x01,0x8b,0x50,0x7c,0x53}},
    {0x100E621F,{0x01,0x5e,0x08,0x8b,0xf8,0x75,0x10}},
    {0x104D5530,{0x56,0x8b,0xf1,0xf7,0x46,0x70,0x00,0x10,0x00,0x00}},
    {0x10068D10,{0x56,0x8b,0xf1,0x83,0x7e,0x64,0x00,0x74,0x18,0x8b,0x46,0x60,0x8b,0x08,0x8b,0x11,0x8b,0x42,0x08,0xff,0xd0,0x83,0xf8,0x07,0x75,0x07,0x8b,0x4e,0x60,0x8b,0x01,0x5e,0xc3,0x33,0xc0,0x5e,0xc3}},
    {0x100D7910,{0x8b,0x44,0x24,0x08,0x8b,0x49,0x10,0x56,0x8b,0x74,0x24,0x08,0x50,0x56,0xe8,0x8d,0xf7,0xff,0xff,0x8b,0xc6,0x5e,0xc2,0x08,0x00}},
    {0x100D70B0,{0x8b,0x51,0x10,0x83,0xec,0x08,0x33,0xc0,0x85,0xd2,0x56,0x76,0x15,0x8b,0x71,0x0c,0x8d,0x4e,0x10,0x83,0x39,0x01,0x74,0x1d,0x83,0xc0,0x01,0x83,0xc1,0x50,0x3b,0xc2,0x72,0xf1}},
    {0x1048529D,{0x89,0x7e,0x54}},
    {0x10485901,{0x89,0x7e,0x5c,0xc6,0x46,0x3c,0x01,0xe8,0x53,0x91,0x01,0x00}},
    {0x1049EA78,{0x89,0xbe,0x80,0x01,0x00,0x00}},
    {0x104B9D97,{0xf3,0x0f,0x10,0x56,0x2c,0xf3,0x0f,0x5c,0xd4,0xf3,0x0f,0x59,0xd3,0xf3,0x0f,0x10,0x5c,0x24,0x0c,0xf3,0x0f,0x58,0xd4}},
    {0x104B9DAE,{0x0f,0x28,0xe3,0x0f,0x54,0xe5}},
    {0x104B9E49,{0x8b,0x55,0x14,0xf3,0x0f,0x11,0x46,0x1c,0xd9,0x46,0x1c,0x5f,0xd9,0x1a,0x5e,0x8b,0xe5,0x5d,0xc2,0x10,0x00}}
};
static bool Range(size_t offset,size_t bytes,size_t length) { return offset<=length && bytes<=length-offset; }
static bool ReadPe(const wchar_t* path,uint8_t* image) {
    std::ifstream input(std::filesystem::path(path),std::ios::binary);
    if(!input) return false;
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    IMAGE_DOS_HEADER dos{};
    if(!Range(0,sizeof dos,file.size())) return false;
    std::memcpy(&dos,file.data(),sizeof dos);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0) return false;
    const size_t ntOffset=size_t(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt{};
    if(!Range(ntOffset,sizeof nt,file.size())) return false;
    std::memcpy(&nt,file.data()+ntOffset,sizeof nt);
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386
        || nt.FileHeader.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER32)
        || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt.OptionalHeader.ImageBase!=PreferredBase || nt.OptionalHeader.SizeOfImage!=ImageBytes
        || !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96
        || !Range(0,nt.OptionalHeader.SizeOfHeaders,file.size())
        || !Range(0,nt.OptionalHeader.SizeOfHeaders,ImageBytes)) return false;
    std::memcpy(image,file.data(),nt.OptionalHeader.SizeOfHeaders);
    const size_t sections=ntOffset+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    if(!Range(sections,size_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER),file.size())) return false;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section,file.data()+sections+size_t(i)*sizeof section,sizeof section);
        if(!Range(section.PointerToRawData,section.SizeOfRawData,file.size())
            || !Range(section.VirtualAddress,section.SizeOfRawData,ImageBytes)) return false;
        if(section.SizeOfRawData) std::memcpy(image+section.VirtualAddress,file.data()+section.PointerToRawData,section.SizeOfRawData);
    }
    return true;
}
static unsigned patchCalls=0;
static uintptr_t patchSite=0;
static size_t patchBytes=0;
static void* patchCode=nullptr;
static bool patchAccept=true,patchExpected=false;
static bool FakePatch(uintptr_t site,const uint8_t* expected,size_t bytes,void* target,void** trampoline) {
    ++patchCalls;patchSite=site;patchBytes=bytes;patchCode=target;
    patchExpected=site && bytes==6 && std::memcmp((const void*)site,expected,bytes)==0;
    *trampoline=nullptr;
    return patchAccept; // Never writes the image, and never executes the generated stub.
}
static void ResetInstall() {
    controls::g_c={};controls::g_nativeSteeringOwnerReady=false;controls::g_steeringResponseEnabled=false;
    controls::translatedCalls=0;gameaddr::version=gameaddr::GOG;
    patchCalls=0;patchSite=0;patchBytes=0;patchCode=nullptr;patchAccept=true;patchExpected=false;
}
static uint32_t Digest(const uint8_t* data,size_t length) {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<length;++i) hash=(hash^data[i])*16777619u;
    return hash;
}
static uintptr_t RelativeDestination(const uint8_t* code,size_t at) {
    int32_t displacement=0;std::memcpy(&displacement,code+at+1,4);
    return uintptr_t(code)+at+5+uintptr_t(displacement);
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace controls;
    static_assert(sizeof(uintptr_t)==4,"native installer fixture requires Win32");
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",name);}};
    auto image=(uint8_t*)VirtualAlloc(nullptr,ImageBytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!image) return 2;
    const bool realImage=argc>1;
    if(realImage) {
        if(!ReadPe(argv[1],image)) { std::fprintf(stderr,"Cannot map supplied GOG PE as data\n");VirtualFree(image,0,MEM_RELEASE);return 2; }
    } else for(const auto& window:windows) std::memcpy(image+window.va-PreferredBase,window.bytes.data(),window.bytes.size());
    DWORD previous=0;
    if(!VirtualProtect(image,ImageBytes,PAGE_READONLY,&previous)) { VirtualFree(image,0,MEM_RELEASE);return 2; }
    g_base=uintptr_t(image);
    const uint32_t originalDigest=Digest(image,ImageBytes);
    check(GA(0x100E6160)==0,"ordinary translator refuses already-GOG fixture addresses");
    ResetInstall();
    check(NGA(0x100E6160)==g_base+0xE6160,"GOG native getter resolves directly by RVA");
    check(NGA(0x104B9DAE)==g_base+0x4B9DAE,"GOG hook seam resolves directly by RVA");
    check(NGA(PreferredBase)==g_base,"image first byte accepted");
    check(NGA(PreferredBase+ImageBytes-1)==g_base+ImageBytes-1,"image last byte accepted");
    check(!NGA(PreferredBase-1)&&!NGA(PreferredBase+ImageBytes),"out-of-image addresses rejected");
    check(!fc2vr::native::GogImageAddress(0,true,0x100E6160),"missing image base rejected");
    check(!fc2vr::native::GogImageAddress(g_base,false,0x100E6160),"non-GOG image rejected");
    check(!fc2vr::native::GogImageAddress(std::numeric_limits<uintptr_t>::max()-4,true,0x100E6160),"address arithmetic overflow rejected");
    for(const auto& window:windows)
        check(NativeOwnerCode(window.va,window.bytes.data(),window.bytes.size()),"pinned guard bytes match mapped image");
    check(InitNativeSteeringOwner(),"actual owner initializer passes all GOG guards");
    check(!ResolveSteeringOwner(nullptr).component,"null owner query never invokes mapped game code");
    InstallMotionSteeringResponse(FakePatch);
    check(g_steeringResponseEnabled&&patchCalls==1,"actual production installer reaches one patch call");
    check(patchSite==g_base+0x4B9DAE&&patchBytes==6&&patchExpected,"patch uses exact GOG seam and verified bytes");
    check(translatedCalls==0,"owner and response installers never double-translate GOG addresses");
    if(patchCode) {
        const auto emitted=fc2vr::motion::MakeSteeringResponseStub(uint32_t(uintptr_t(patchCode)),uint32_t(uintptr_t(&NativeSteeringResponse)),uint32_t(g_base+0x4B9DB4),uint32_t(g_base+0x4B9E49));
        check(!std::memcmp(patchCode,emitted.bytes.data(),emitted.size),"installed code uses direct GOG continuations and actual callback");
        const auto code=(const uint8_t*)patchCode;
        // The final two branches are accepted-path E9 then native fallback E9.
        check(emitted.size>=20&&code[emitted.size-5]==0xE9&&RelativeDestination(code,emitted.size-5)==g_base+0x4B9DB4,"fallback continuation is actual GOG instruction");
        check(emitted.size>=20&&code[emitted.size-20]==0xE9&&RelativeDestination(code,emitted.size-20)==g_base+0x4B9E49,"accepted continuation is actual native writeback");
        MEMORY_BASIC_INFORMATION info{};VirtualQuery(patchCode,&info,sizeof info);
        check(info.Protect==PAGE_EXECUTE_READ,"generated stub finalized read-only executable");
        VirtualFree(patchCode,0,MEM_RELEASE);
    }
    check(Digest(image,ImageBytes)==originalDigest,"installation fixture leaves mapped image unchanged");
    // Mutate only our private data mapping, one independently pinned guard at a
    // time, then restore it. The optional source DLL stays unopened for writes.
    for(const auto& window:windows) {
        ResetInstall();auto byte=image+window.va-PreferredBase;
        VirtualProtect(byte,1,PAGE_READWRITE,&previous);*byte^=1;VirtualProtect(byte,1,PAGE_READONLY,&previous);
        InstallMotionSteeringResponse(FakePatch);
        check(!g_steeringResponseEnabled&&!patchCalls,"any corrupted guard prevents installation");
        VirtualProtect(byte,1,PAGE_READWRITE,&previous);*byte^=1;VirtualProtect(byte,1,PAGE_READONLY,&previous);
    }
    ResetInstall();patchAccept=false;InstallMotionSteeringResponse(FakePatch);
    check(patchCalls==1&&!g_steeringResponseEnabled,"patch rejection keeps hook disabled");
    MEMORY_BASIC_INFORMATION freed{};VirtualQuery(patchCode,&freed,sizeof freed);
    check(freed.State==MEM_FREE,"rejected patch frees generated stub allocation");
    ResetInstall();gameaddr::version=gameaddr::Steam;InstallMotionSteeringResponse(FakePatch);
    check(!patchCalls&&!g_steeringResponseEnabled,"non-GOG build never patches");
    ResetInstall();g_c.steeringDirect=false;InstallMotionSteeringResponse(FakePatch);
    check(!patchCalls,"disabled direct response never patches");
    ResetInstall();g_c.motionSteering=false;InstallMotionSteeringResponse(FakePatch);
    check(!patchCalls,"disabled motion steering never patches");
    ResetInstall();InstallMotionSteeringResponse(nullptr);check(!patchCalls,"missing patch adapter never patches");
    check(Digest(image,ImageBytes)==originalDigest,"all guard trials restore the complete data image");
    MEMORY_BASIC_INFORMATION mapped{};VirtualQuery(image,&mapped,sizeof mapped);
    check(mapped.Protect==PAGE_READONLY,"game image remains non-executable read-only data");
    g_base=0;VirtualFree(image,0,MEM_RELEASE);
    std::printf("native steering install (%s): %d checks, %d failures\n",realImage?"disk PE data":"synthetic image",checks,failures);
    return failures?1:0;
}
