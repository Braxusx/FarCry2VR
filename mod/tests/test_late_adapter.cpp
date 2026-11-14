// Win32 adapter integration harness: includes the production hook bodies, but
// supplies owned byte buffers and deterministic native-call stand-ins. No game
// process, installed DLL, native address, or hook installation is accessed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "arms.h"
#include "common/late_pose.h"
#include "common/support_pose.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

static_assert(sizeof(void*)==4,"native adapter layout requires a Win32 test target");
namespace harness {
static ULONGLONG clockMs=1005;
static ULONGLONG Clock() { return clockMs; }
template<class... T> void Log(const char*,T...) {}
static bool normal=true,mapOpen=false,mapControls=false,phone=false,loading=false;
static int vehicle=0,lockDepth=0;
static int depthCalls=0;
}
namespace controls {
static bool NormalControls() { return harness::normal; }
static bool MapOpen() { return harness::mapOpen; }
static bool MapControls() { return harness::mapControls; }
static bool PhoneOut() { return harness::phone; }
static bool LoadingControls() { return harness::loading; }
static int VehicleMode() { return harness::vehicle; }
}
namespace gameaddr {
enum { GOG=1 };
static int Detect() { return GOG; }
}
namespace arms {
struct M4 { float m[4][4]; };
struct Bone { uint32_t hash;uint8_t pad0[12];float local[16],model[16];int parent;uint8_t locked,pad1[11]; };
static_assert(sizeof(Bone)==0xA0,"worker bone stride must match production");
static uintptr_t g_base=0x10000000;
static Config g_c;
static Frame g_frame;
static void* g_curWeapon=nullptr;
static uint8_t* g_wGfx=nullptr;
static volatile uint8_t* g_playerGfx=nullptr;
static int g_slot[21]{},g_follow[512]{};
static bool (*g_captureActive)()=nullptr;
static void (*g_captureRecord)(char,const void*,uint32_t)=nullptr;
static fc2vr::tracking::Vec3 Read3(const float* p) { return {p[0],p[1],p[2]}; }
static void Lock() { ++harness::lockDepth; }
static void Unlock() { --harness::lockDepth; }
static bool g_nearViewport=true;
static void WorldBodyOcclusion(uint8_t*,uint8_t*,const char*) { ++harness::depthCalls; }
#include "arms_gadget_depth.inl"
bool ReloadActive() { return false; }
#define GetTickCount64 harness::Clock
#define LOG(...) harness::Log(__VA_ARGS__)
#if defined(_MSC_VER)
#define __builtin_return_address(index) nullptr
#endif
#include "arms_late.inl"
#if defined(_MSC_VER)
#undef __builtin_return_address
#endif
#undef LOG
#undef GetTickCount64
}

namespace harness {
using namespace fc2vr::presentation;
using arms::Frame;
namespace adapter=arms::late;
static int checks=0,failures=0,workerCalls=0,fillCalls=0,cacheCalls=0,getCalls=0;
static constexpr unsigned Bones=4;
static constexpr uint64_t Handle=0x123456789ABCDEF0ull;
// Same resource index with a different high-word generation.
static constexpr uint64_t OtherHandle=0x123456799ABCDEF0ull;
static std::array<uint8_t,0x950> provider{};
static std::array<uint8_t,0x20> cacheObject{},resource{};
static std::array<uint8_t,0x140> gfx{};
static std::array<uint8_t,16> skeleton{};
static std::array<arms::Bone,Bones> bones{},foreignBones{};
static std::array<Matrix,Bones> output{},bind{},originalOutput{};
static std::array<int32_t,Bones> workerRemap{{2,0,3,1}};
static std::array<uint16_t,Bones> subsetIndices{{1,3,0,2}};
static std::array<uint8_t,8> subset{};
static uint64_t dirtyBits=0;
static adapter::Source originalSource{};
static void Check(bool ok,const char* message) {
    ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",message); }
}
template<class T,size_t N> static void Store(std::array<uint8_t,N>& bytes,size_t offset,T value) {
    std::memcpy(bytes.data()+offset,&value,sizeof value);
}
static Matrix Model(float x,float y,float z) {
    auto m=Identity();m[12]=x;m[13]=y;m[14]=z;return m;
}
static void Put(float* dst,Vec3 value) { dst[0]=value.x;dst[1]=value.y;dst[2]=value.z; }
static uint8_t* __fastcall NativeGet(void* pool,void*,const uint64_t* handle) {
    ++getCalls;
    // The pool is an identity only. Never dereference a game VA in this harness.
    if(pool!=(void*)adapter::VA(0x10EF0720) || !handle || (*handle!=Handle && *handle!=OtherHandle)) return nullptr;
    return resource.data();
}
static void __cdecl NativeWorker(Matrix* dst,const arms::Bone* src,const Matrix* inverseBind,
                                  const int32_t* remap,unsigned count,bool skipBind) {
    ++workerCalls;
    for(unsigned i=0;i<count;++i) {
        Matrix model;std::memcpy(model.data(),src[remap[i]].model,64);
        dst[i]=skipBind?model:Multiply(inverseBind[i],model);
    }
}
static uint8_t* __fastcall NativeCache(uint8_t*,void*,const uint64_t*) { ++cacheCalls;return provider.data(); }
static void __fastcall NativeFill(uint8_t* dst,void*,const uint64_t* handle,const uint8_t* remap) {
    ++fillCalls;
    if(!handle || (*handle!=Handle && *handle!=OtherHandle)) return;
    const auto indices=*reinterpret_cast<uint16_t* const*>(remap);
    const unsigned count=*reinterpret_cast<const unsigned*>(remap+4);
    if(count>Bones) return;
    for(unsigned i=0;i<count;++i) {
        if(indices[i]>=Bones) return;
        Packed p{};Pack(output[indices[i]],p);std::memcpy(dst+0x30+i*48,p.data(),48);
    }
    dst[0x940]=0;
}
static void Reset() {
    for(auto& p:adapter::poses) p=adapter::Pose{};
    for(auto& s:adapter::skins) s=adapter::Skin{};
    for(auto& w:adapter::worlds) w=adapter::World{};
    for(auto& w:adapter::markerWorlds) w=adapter::World{};
    adapter::markerWorldNext=0;
    for(auto& entry:adapter::mapGraphics) entry=adapter::MapGraphics{};
    adapter::mapGraphicsNext=0;
    for(auto& mask:adapter::markerMask) mask=0;
    for(auto& o:adapter::overlays) o=adapter::Overlay{};
    adapter::overlayNext=adapter::overlayViewNext=0;
    adapter::overlaySeen=adapter::overlayMatched=adapter::overlayDraws[0]=adapter::overlayDraws[1]=0;
    adapter::overlayDepthVerified=true;adapter::overlayDepthDraws=0;arms::g_nearViewport=true;
    adapter::nestedMatched=adapter::nestedDraws=0;
    for(auto& c:adapter::cached) c=adapter::DeltaCache{};
    adapter::poseNext=adapter::skinNext=adapter::worldNext=adapter::cacheNext=0;
    adapter::workerSeen=adapter::workerMatched=adapter::skinDraws=0;
    adapter::worldSeen=adapter::worldMatched=adapter::worldDraws=0;
    adapter::installed=true;adapter::eye=adapter::ready=adapter::renderThread=false;
    adapter::skinHandleCount=1;adapter::skinHandles[0]=Handle;
    adapter::originalGet=NativeGet;adapter::originalWorker=NativeWorker;
    adapter::originalFill=NativeFill;adapter::originalCache=NativeCache;
    clockMs=1005;normal=true;mapOpen=mapControls=phone=loading=false;vehicle=lockDepth=0;
    workerCalls=fillCalls=cacheCalls=getCalls=0;dirtyBits=0;
    depthCalls=0;arms::g_gadgetDepthNext=0;
    for(auto& item:arms::g_gadgetDepth) item=arms::GadgetDepthEntry{};
    arms::g_slot[12]=1;arms::g_slot[20]=0;
    adapter::phoneDraws=adapter::gadgetDraws=0;
    provider.fill(0xCD);cacheObject.fill(0);resource.fill(0);gfx.fill(0);
    Store(provider,0x24,&dirtyBits);provider[0x28]=3;provider[0x940]=0;
    Store(cacheObject,4,Handle);Store(resource,4,output.data());Store(resource,8,Bones);
    Store(subset,0,subsetIndices.data());Store(subset,4,Bones);
    Frame f{};f.valid=f.handValid=f.leftValid=f.headValid=true;
    f.handAuthority=f.leftAuthority=f.headAuthority=true;
    f.bonePubValid=f.bonePubRoot=true;f.roomEpoch=7;f.frameSequence=10;
    f.bonePubSkeleton=reinterpret_cast<uintptr_t>(skeleton.data());
    f.bonePubGraphics=reinterpret_cast<uintptr_t>(gfx.data());
    f.weapon=reinterpret_cast<void*>(0x1234);f.game.P={0,0,1.65f};
    f.source.sequence=100;f.source.receivedMs=1000;f.source.generation=3;
    f.source.handGeneration[0]=4;f.source.handGeneration[1]=5;
    f.source.focused=f.source.synced=true;
    Put(f.aimR,{1,0,0});Put(f.aimU,{0,1,0});Put(f.aimB,{0,0,1});
    Put(f.laimR,{1,0,0});Put(f.laimU,{0,1,0});Put(f.laimB,{0,0,1});
    Put(f.gripO,{.3f,-.3f,-.45f});Put(f.lgripO,{-.3f,-.3f,-.45f});
    Put(f.headO,{0,0,0});Put(f.headF,{0,0,-1});
    originalSource={};originalSource.frame=f;originalSource.root=Model(100,200,3);
    originalSource.body=77;originalSource.skeleton=f.bonePubSkeleton;
    originalSource.graphics=f.bonePubGraphics;originalSource.hand[0]=0;originalSource.hand[1]=1;
    const Matrix models[Bones]={Model(-.3f,.45f,1.35f),Model(.3f,.45f,1.35f),
        Model(0,.1f,1.1f),Model(0,-.1f,.2f)};
    auto& pose=adapter::poses[0];pose.source=originalSource;pose.workerBones=bones.data();pose.count=Bones;
    for(unsigned i=0;i<Bones;++i) {
        bones[i]={};std::memcpy(bones[i].model,models[i].data(),64);
        bind[i]=Model(.01f*float(i+1),-.02f*float(i),.03f*float(i));
        pose.models[i]=models[i];pose.side[i]=i<3?static_cast<int8_t>(i):-1;
    }
    foreignBones=bones;adapter::poseNext=1;adapter::ownerBones=bones.data();
    arms::g_playerGfx=gfx.data();arms::g_curWeapon=f.weapon;
    arms::g_frame=f;
}
static void Capture(bool skipBind=false) {
    const auto before=bones;
    adapter::Worker(output.data(),bones.data(),bind.data(),workerRemap.data(),Bones,skipBind);
    originalOutput=output;
    Check(std::memcmp(before.data(),bones.data(),sizeof bones)==0,"worker hook leaves native bones unchanged");
    Check(workerCalls==1 && adapter::workerMatched==1 && adapter::skinNext==1,"worker matches copied local pose and original output");
}
static void Target(unsigned step=1) {
    auto f=originalSource.frame;f.game.P=fc2vr::tracking::Add(f.game.P,{100+.01f*step,200,3});
    f.gripO[0]+=.08f*step;f.lgripO[2]+=.06f*step;f.headO[1]+=.03f*step;
    f.frameSequence+=step;f.source.sequence+=step;f.source.receivedMs=clockMs;
    arms::g_frame=f;arms::PreparePresentation(true);
    Check(adapter::ready,"valid owned target prepares the production adapter");
}
static std::array<Packed,Bones> Draw(uint64_t handle=Handle) {
    Store(cacheObject,4,handle);
    const auto p=adapter::Cache(cacheObject.data(),nullptr,&handle);
    if(p[0x940]) adapter::Fill(p,nullptr,&handle,subset.data());
    std::array<Packed,Bones> result{};std::memcpy(result.data(),p+0x30,sizeof result);return result;
}
static std::array<Packed,Bones> NativePayload() {
    std::array<Packed,Bones> expected{};
    for(unsigned i=0;i<Bones;++i) Pack(originalOutput[subsetIndices[i]],expected[i]);
    return expected;
}
static void CheckExpected(const std::array<Packed,Bones>& actual,unsigned step) {
    auto expected=NativePayload();
    // Independent translation oracle; the fixture controllers have identity
    // rotations and each palette slot maps to a different model-space bone.
    expected[0][3]+=.01f*step;expected[0][7]-=.06f*step;
    expected[1][3]+=.09f*step;
    expected[2][3]+=.01f*step;expected[2][11]+=.03f*step;
    for(unsigned i=0;i<Bones;++i) for(unsigned j=0;j<12;++j)
        Check(std::isfinite(actual[i][j]) && std::fabs(actual[i][j]-expected[i][j])<.00002f,
              "provider copy follows current independent hand/head target and preserves native leg");
}
static void HappyAndRepeated() {
    for(bool skipBind:{false,true}) for(bool palm:{false,true}) {
        Reset();
        originalSource.frame.runtimePalm[0]=originalSource.frame.runtimePalm[1]=palm;
        adapter::poses[0].source=originalSource;
        Capture(skipBind);const auto originalBones=bones;Target();
        arms::BeginPresentationEye();const auto left=Draw();arms::EndPresentationEye();
        arms::BeginPresentationEye();const auto right=Draw();arms::EndPresentationEye();
        CheckExpected(left,1);Check(left==right,"both eye fills are identical from the same original source");
        Check(adapter::skinDraws==2 && fillCalls==2,"cache forces native refill on both eyes");
        Check((dirtyBits&(uint64_t(1)<<3))!=0,"corrected provider marks its native dirty bit");
        Target(2);arms::BeginPresentationEye();const auto newer=Draw();arms::EndPresentationEye();
        CheckExpected(newer,2);Check(newer!=left,"reused worker source follows a newer target without accumulating corrections");
        Check(output==originalOutput,"render correction leaves worker output byte-identical");
        Check(std::memcmp(originalBones.data(),bones.data(),sizeof bones)==0,"render correction leaves native bones byte-identical");
        Check(provider[0x2F]==0xCD && provider[0x30+Bones*48]==0xCD,"packed provider writes preserve neighboring bytes");
        Check(Draw()==NativePayload(),"non-eye cache refill restores native data after a corrected draw");
        arms::PreparePresentation(false);arms::BeginPresentationEye();
        Check(Draw()==NativePayload(),"disabled presentation restores native cache contents");arms::EndPresentationEye();
        Check(lockDepth==0,"production adapter releases every simulated lock");
    }
}
static void OwnershipAndFreshness() {
    Reset();Capture();Target();arms::BeginPresentationEye();
    provider[0x940]=0;const auto before=provider;
    Draw(OtherHandle);
    Check(provider==before,"foreign resource handle cannot force a provider refill");
    adapter::Fill(provider.data(),nullptr,&OtherHandle,subset.data());
    Check(adapter::skinDraws==0,"foreign full handle cannot acquire local correction");
    Check(std::memcmp(provider.data()+0x30,NativePayload().data(),Bones*48)==0,"foreign fill retains native data");
    arms::EndPresentationEye();

    for(unsigned invalid=0;invalid<9;++invalid) {
        Reset();Capture();Target();
        if(invalid==0) { clockMs=1200;arms::g_frame.source.receivedMs=clockMs; }
        if(invalid==1) ++arms::g_frame.source.generation;
        if(invalid==2) ++arms::g_frame.source.handGeneration[0];
        if(invalid==3) ++arms::g_frame.bonePubSkeleton;
        if(invalid==4) ++arms::g_frame.roomEpoch;
        if(invalid==5) arms::g_frame.weapon=reinterpret_cast<void*>(0x5678);
        if(invalid==6) ++arms::g_frame.bonePubGraphics;
        if(invalid==7) arms::g_frame.runtimePalm[0]=true;
        if(invalid==8) arms::g_frame.runtimePalm[1]=true;
        arms::PreparePresentation(true);arms::BeginPresentationEye();
        Check(Draw()==NativePayload(),"stale or mismatched scene ownership rejects correction");
        Check(adapter::skinDraws==0,"rejected context never increments corrected draw count");arms::EndPresentationEye();
    }

    Reset();
    adapter::Worker(output.data(),foreignBones.data(),bind.data(),workerRemap.data(),Bones,false);
    Check(workerCalls==1 && adapter::skinNext==0,"nonlocal bones execute native worker without acquiring local ownership");
    Reset();adapter::poses[0].models[0][12]+=.02f;
    adapter::Worker(output.data(),bones.data(),bind.data(),workerRemap.data(),Bones,false);
    Check(adapter::skinNext==0,"changed consumed bone matrix rejects the stored source stamp");
    Reset();Capture();Target();output[0][12]+=.01f;originalOutput=output;
    arms::BeginPresentationEye();Check(Draw()==NativePayload(),"changed raw output bytes reject a reused pointer");arms::EndPresentationEye();
}
static void InvalidTargetAndDisabledModes() {
    for(unsigned invalid=0;invalid<8;++invalid) {
        Reset();Capture();Target();arms::BeginPresentationEye();
        const auto corrected=Draw();arms::EndPresentationEye();
        Check(corrected!=NativePayload(),"rejection case begins with a corrected cached provider");
        if(invalid==0) arms::g_frame.gripO[0]+=1;
        if(invalid==1) arms::g_frame.headF[0]=std::numeric_limits<float>::quiet_NaN();
        if(invalid==2) arms::g_frame.handAuthority=false;
        if(invalid==3) arms::g_frame.leftValid=false;
        if(invalid==4) arms::g_frame.twoHand=true;
        if(invalid==5) arms::g_frame.map=true;
        if(invalid==6) normal=false;
        if(invalid==7) vehicle=1;
        arms::PreparePresentation(true);arms::BeginPresentationEye();
        Check(Draw()==NativePayload(),"invalid target or disabled handling mode restores every palette entry to native");
        Check(adapter::skinDraws==1,"failed target cannot publish a partial palette correction");
        Check(output==originalOutput,"fail-closed path preserves the original worker output");
        arms::EndPresentationEye();Check(lockDepth==0,"rejected target releases its simulated lock");
    }
}
static void Ambiguity() {
    Reset();adapter::poses[1]=adapter::poses[0];adapter::poses[1].source.body=78;
    adapter::poses[1].source.frame.gripO[0]+=.03f;adapter::poseNext=2;
    adapter::Worker(output.data(),bones.data(),bind.data(),workerRemap.data(),Bones,false);
    Check(workerCalls==1 && adapter::workerSeen==1 && adapter::workerMatched==0 && adapter::skinNext==0,
        "same native matrices with two different controller sources reject ambiguous attribution");
    Reset();adapter::poses[1]=adapter::poses[0];adapter::poses[1].source.body=78;adapter::poseNext=2;
    Capture();Target();arms::BeginPresentationEye();CheckExpected(Draw(),1);arms::EndPresentationEye();
    Check(adapter::skinDraws==1,"equivalent duplicate source matrices remain usable");

    // Skin history outlives pose history. Reusing a worker pointer and identical
    // raw matrices does not identify one controller source across that window.
    Reset();Capture();Target();adapter::skins[1]=adapter::skins[0];
    adapter::skins[1].source.body=78;
    adapter::skins[1].source.frame.gripO[0]+=.03f;adapter::skinNext=2;
    arms::BeginPresentationEye();
    Check(Draw()==NativePayload(),"retained skins with identical pointer and bytes but conflicting sources reject attribution");
    Check(adapter::skinDraws==0,"retained skin ambiguity never publishes a correction");arms::EndPresentationEye();

    Reset();Capture();Target();adapter::skins[1]=adapter::skins[0];
    adapter::skins[1].source.body=78;adapter::skinNext=2;
    arms::BeginPresentationEye();CheckExpected(Draw(),1);arms::EndPresentationEye();
    Check(adapter::skinDraws==1,"equivalent retained skin duplicates remain usable");
}
#include "test_supported_adapter.inl"
#include "test_overlay_adapter.inl"
#include "test_gadget_adapter.inl"
}
int main() {
    // These production functions compile and retain their actual bodies, but
    // the test never installs hooks or follows native publication addresses.
    (void)&arms::RememberPresentation;(void)&arms::InstallPresentation;
    harness::HappyAndRepeated();harness::OwnershipAndFreshness();
    harness::InvalidTargetAndDisabledModes();harness::Ambiguity();
    harness::SupportedAndMapSkin();harness::AttachedProviders();
    harness::OverlayDisplays();harness::OverlayRejection();harness::NestedMapMarkers();harness::MapCompositorAlignment();
    harness::PhoneAndGadgetPresentation();harness::GadgetDepthOwnership();
    std::printf("late adapter: %d checks, %d failures\n",harness::checks,harness::failures);
    return harness::failures?1:0;
}
