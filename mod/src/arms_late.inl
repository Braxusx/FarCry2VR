// GOG 1.03 only. See docs/PRESENTATION.md for native ABI/provenance evidence.
// Included in arms.cpp after native ownership helpers. No skeleton, entity,
// physics, animation, worker-output or published-world writes are made here.
namespace late {
using namespace fc2vr::presentation;
constexpr unsigned History=16, Resources=32, MarkerResources=256, MaxBones=512, MaxSkin=512;
struct Source {
    Frame frame{}; // game.P is ROOT-RELATIVE, following the exact Apply transport
    Matrix root{}, handWorld[2]{};
    SupportHold support{};
    uintptr_t skeleton=0,graphics=0;
    uint64_t body=0;
    int hand[2]={-1,-1};
};
struct Pose { Source source{}; Bone* workerBones=nullptr; unsigned count=0; Matrix models[MaxBones]{}; int8_t side[MaxBones]{}; };
struct Skin { Source source{}; const Matrix* output=nullptr; unsigned count=0; Matrix raw[MaxSkin]{}; int8_t side[MaxSkin]{}; };
struct World { Source source{}; uint64_t handle=0; Matrix raw{}; Vec3 fraction{}; int side=1; bool nested=false; };
static Pose poses[History]; static unsigned poseNext=0;
static Skin skins[Resources]; static unsigned skinNext=0;
static World worlds[Resources]; static unsigned worldNext=0;
static World markerWorlds[MarkerResources]; static unsigned markerWorldNext=0;
static uint64_t skinHandles[64]{};static unsigned skinHandleCount=0;
static Bone* volatile ownerBones=nullptr;
static bool installed=false;
static thread_local Frame render;
static thread_local bool ready=false,eye=false,renderThread=false;
struct DeltaCache { uint64_t body=0;Matrix delta[3]{},world[2]{};bool valid=false; };
static thread_local DeltaCache cached[History];static thread_local unsigned cacheNext=0;
static volatile LONG workerSeen=0,workerMatched=0,skinDraws=0,worldSeen=0,worldMatched=0,worldDraws=0;
static volatile LONG supportedDraws=0,mapDraws=0,worldHandDraws[2]{};
static volatile LONG phoneDraws=0,gadgetDraws=0;
static uintptr_t VA(uintptr_t va) { return g_base+va-0x10000000; }
static Matrix Read(const void* pointer) { Matrix m;memcpy(m.data(),pointer,64);return m; }
static Matrix Affine(Matrix m) { m[3]=m[7]=m[11]=0;m[15]=1;return m; }
static bool Near(const Matrix& a,const Matrix& b,float translation=0.00001f) {
    for(unsigned i=0;i<16;++i) {
        const float tolerance=i>=12 && i<15 ? translation : 0.00001f;
        if(!std::isfinite(a[i])||!std::isfinite(b[i])||fabsf(a[i]-b[i])>tolerance) return false;
    }
    return true;
}
static Matrix ControllerAt(const Frame& f,int side,Vec3 origin) {
    if(side<2) return Controller(f.game,Read3(side?f.aimR:f.laimR),Read3(side?f.aimU:f.laimU),
        Read3(side?f.aimB:f.laimB),Read3(side?f.gripO:f.lgripO),origin);
    // Upright head yaw transports the upper torso; planted legs stay native.
    float x=f.headF[0],z=f.headF[2],length=sqrtf(x*x+z*z);
    if(!std::isfinite(length)||length<0.1f) return detail::Invalid();
    x/=length;z/=length;
    return Controller(f.game,{-z,0,x},{0,1,0},{-x,0,-z},Read3(f.headO),origin);
}
static bool ControllersAt(const Frame& f,const SupportHold& hold,Vec3 origin,Matrix out[3]) {
    out[0]=ControllerAt(f,0,origin);out[1]=ControllerAt(f,1,origin);out[2]=ControllerAt(f,2,origin);
    if(!f.twoHand) return hold.mode==SupportMode::None;
    return hold.mode!=SupportMode::None && SupportedControllers(out[1],out[0],hold,out[1],out[0]);
}
static bool ModeAllowed(const Frame& f) {
    if(controls::VehicleMode() || controls::LoadingControls()) return false;
    if(f.phone) return controls::PhoneOut() && !f.map && !f.weapon && !f.twoHand
        && (controls::NormalControls() || controls::MapControls());
    if(controls::PhoneOut()) return false;
    return f.map ? controls::MapControls() && controls::MapOpen() && !f.twoHand : controls::NormalControls();
}
static bool Equivalent(const Source& a,const Source& b) {
    if(a.frame.weapon!=b.frame.weapon || a.frame.roomEpoch!=b.frame.roomEpoch || !Near(a.root,b.root)
        || a.frame.map!=b.frame.map || a.frame.phone!=b.frame.phone || a.frame.twoHand!=b.frame.twoHand || a.support.mode!=b.support.mode
        || a.frame.runtimePalm[0]!=b.frame.runtimePalm[0] || a.frame.runtimePalm[1]!=b.frame.runtimePalm[1]) return false;
    const Vec3 av[]={a.support.pivot,a.support.foregrip},bv[]={b.support.pivot,b.support.foregrip};
    for(int i=0;i<2;++i) {
        const auto d=fc2vr::tracking::Subtract(av[i],bv[i]);
        if(!detail::Finite(d)||fc2vr::tracking::DotProduct(d,d)>1e-10f) return false;
    }
    for(int side=0;side<3;++side) if(!Near(ControllerAt(a.frame,side,{}),ControllerAt(b.frame,side,{}))) return false;
    return true;
}
static bool Compatible(const Source& s, bool hudLayer=false) {
    return (eye || (hudLayer && ready)) && render.valid && s.body && s.skeleton==render.bonePubSkeleton && s.graphics==render.bonePubGraphics
        && s.frame.weapon==render.weapon && s.frame.roomEpoch==render.roomEpoch
        && s.frame.runtimePalm[0]==render.runtimePalm[0] && s.frame.runtimePalm[1]==render.runtimePalm[1]
        && s.frame.twoHand==render.twoHand && s.frame.map==render.map && s.frame.phone==render.phone && s.frame.leftValid && render.leftValid
        && s.frame.headValid && render.headValid && !s.frame.bonePubSeat
        && fc2vr::authority::Fresh(s.frame.source,render.source,GetTickCount64(),0)
        && fc2vr::authority::Fresh(s.frame.source,render.source,GetTickCount64(),1);
}
static bool Deltas(const Source& s,Matrix out[3],Matrix* worldDelta=nullptr,int attachmentSide=1,bool hudLayer=false) {
    if(!Compatible(s,hudLayer)) return false;
    if(attachmentSide<0 || attachmentSide>1) return false;
    for(const auto& entry:cached) if(entry.body==s.body) {
        if(!entry.valid) return false;
        memcpy(out,entry.delta,sizeof entry.delta);if(worldDelta) *worldDelta=entry.world[attachmentSide];return true;
    }
    auto& entry=cached[cacheNext++%History];entry=DeltaCache{};entry.body=s.body;
    Matrix rotation=s.root;rotation[12]=rotation[13]=rotation[14]=0;
    const Vec3 origin{s.root[12],s.root[13],s.root[14]};
    Matrix old[3],now[3];
    if(!ControllersAt(s.frame,s.support,{},old)||!ControllersAt(render,s.support,origin,now)) return false;
    for(int side=0;side<3;++side) {
        if(!Delta(old[side],now[side],rotation,entry.delta[side])) return false;
        if(side<2 && !Delta(old[side],now[side],Identity(),entry.world[side])) return false;
    }
    entry.valid=true;memcpy(out,entry.delta,sizeof entry.delta);if(worldDelta) *worldDelta=entry.world[attachmentSide];return true;
}
static uint8_t* Entity(uint8_t* gfx) {
    if(!gfx || IsBadReadPtr(gfx+8,4)) return nullptr;
    auto handle=*(uint8_t**)(gfx+8);
    return handle && !IsBadReadPtr(handle+12,4) ? *(uint8_t**)(handle+12) : nullptr;
}
static bool Dirty(uint8_t* provider) {
    if(!provider || IsBadReadPtr(provider+0x24,5)) return false;
    auto bits=*(uint64_t**)(provider+0x24);const unsigned bit=provider[0x28];
    if(bit>=64 || !bits || IsBadWritePtr(bits,8)) return false;
    *bits|=uint64_t(1)<<bit;return true;
}
static void Record(unsigned kind,uint8_t* provider,const Source& s,const Matrix* delta,int side=-1) {
    if(!g_captureActive || !g_captureRecord || !g_captureActive()) return;
    struct Record { uint32_t version,kind,provider,reserved;uint64_t frame,sourceFrame,body;float root[16],delta[48]; } r{};
    r.version=1;r.kind=kind;r.provider=(uint32_t)(uintptr_t)provider;
    r.reserved=(s.frame.twoHand?1u:0u)|(s.frame.map?2u:0u)|(side==0?4u:0u);
    r.frame=render.frameSequence;r.sourceFrame=s.frame.frameSequence;r.body=s.body;
    memcpy(r.root,s.root.data(),64);memcpy(r.delta,delta,192);
    g_captureRecord('L',&r,sizeof r);
}
using Get_t=uint8_t* (__fastcall *)(void*,void*,const uint64_t*);
using SkinWorker_t=void (__cdecl *)(Matrix*,const Bone*,const Matrix*,const int32_t*,unsigned,bool);
using SkinFill_t=void (__fastcall *)(uint8_t*,void*,const uint64_t*,const uint8_t*);
using Cache_t=uint8_t* (__fastcall *)(uint8_t*,void*,const uint64_t*);
using Publish_t=void (__fastcall *)(uint8_t*,void*);
static Get_t originalGet=nullptr;static SkinWorker_t originalWorker=nullptr;
static SkinFill_t originalFill=nullptr;static Cache_t originalCache=nullptr;static Publish_t originalPublish=nullptr;
static Get_t writeGet=nullptr;

static void __cdecl Worker(Matrix* dst,const Bone* bones,const Matrix* bind,const int32_t* remap,unsigned count,bool skipBind) {
    originalWorker(dst,bones,bind,remap,count,skipBind);
    if(!installed || !bones || bones!=ownerBones || !count || count>MaxSkin || !dst || !remap) return;
    InterlockedIncrement(&workerSeen);
    if(IsBadReadPtr(dst,count*64)||IsBadReadPtr(remap,count*4)||(!skipBind && (!bind||IsBadReadPtr(bind,count*64)))) return;
    Lock();const Pose* match=nullptr;bool ambiguous=false;
    for(unsigned p=0;p<History;++p) {
        const auto& candidate=poses[(poseNext+History-1-p)%History];
        if(candidate.workerBones!=bones || !candidate.count) continue;
        bool equal=true;
        for(unsigned i=0;i<count && equal;++i) {
            const int b=remap[i];
            if(b<0 || unsigned(b)>=candidate.count) { equal=false;break; }
            // Match BOTH the consumed model and produced skin matrix. A worker
            // overlapping a newer animation cannot inherit its source stamp.
            if(!Near(Affine(Read(bones[b].model)),candidate.models[b])) { equal=false;break; }
            const auto expected=skipBind ? candidate.models[b] : Multiply(Affine(bind[i]),candidate.models[b]);
            equal=Near(Affine(dst[i]),expected,0.00003f);
        }
        if(!equal) continue;
        if(match && !Equivalent(match->source,candidate.source)) { ambiguous=true;break; }
        if(!match) match=&candidate;
    }
    if(match && !ambiguous) {
        auto& saved=skins[skinNext++%Resources];saved.source=match->source;saved.output=dst;saved.count=count;
        memcpy(saved.raw,dst,count*64);
        for(unsigned i=0;i<count;++i) saved.side[i]=match->side[remap[i]];
        InterlockedIncrement(&workerMatched);
    }
    Unlock();
}
// Called only with g_cs held. Full resource handle comes from the local gfx;
// raw pointer AND raw bytes identify the queued worker output version.
static const Skin* FindSkin(const uint64_t* handle) {
    if(!handle || IsBadReadPtr(handle,8)) return nullptr;
    const uint64_t id=*handle;bool owned=false;
    for(unsigned i=0;i<skinHandleCount;++i) owned=owned || skinHandles[i]==id;
    if(!owned) return nullptr;
    auto resource=originalGet((void*)VA(0x10EF0720),nullptr,handle);
    if(!resource || IsBadReadPtr(resource+4,8)) return nullptr;
    const auto data=*(Matrix**)(resource+4);const unsigned count=*(unsigned*)(resource+8);
    if(!data || !count || count>MaxSkin || IsBadReadPtr(data,count*64)) return nullptr;
    const Skin* match=nullptr;
    for(unsigned i=0;i<Resources;++i) {
        const auto& s=skins[(skinNext+Resources-1-i)%Resources];
        if(s.output==data && s.count==count && Compatible(s.source) && !memcmp(s.raw,data,count*64)) {
            if(match && !Equivalent(match->source,s.source)) return nullptr;
            if(!match) match=&s;
        }
    }
    return match;
}
static uint8_t* __fastcall Cache(uint8_t* self,void* edx,const uint64_t* subset) {
    auto provider=originalCache(self,edx,subset);
    if(!installed || !provider || !self || IsBadReadPtr(self+4,8) || IsBadWritePtr(provider+0x940,1)) return provider;
    Lock();const uint64_t handle=*(uint64_t*)(self+4);
    // Also refill on disabled/non-eye draws: a cached visual correction must
    // never survive a switch to menus, scopes or tracking loss.
    for(unsigned i=0;i<skinHandleCount;++i) if(skinHandles[i]==handle) { provider[0x940]=1;break; }
    Unlock();return provider;
}
static void __fastcall Fill(uint8_t* self,void* edx,const uint64_t* handle,const uint8_t* remap) {
    originalFill(self,edx,handle,remap);
    if(!eye || !self || !remap || IsBadReadPtr(remap,8)) return;
    const auto indices=*(uint16_t**)remap;const unsigned count=*(unsigned*)(remap+4);
    if(!indices || !count || count>48 || IsBadReadPtr(indices,count*2) || IsBadWritePtr(self+0x30,count*48)) return;
    Lock();const auto saved=FindSkin(handle);Matrix delta[3];
    if(saved && Deltas(saved->source,delta)) {
        Packed output[48];bool valid=true;
        for(unsigned i=0;i<count && valid;++i) {
            const unsigned index=indices[i];if(index>=saved->count) { valid=false;break; }
            Packed native{};valid=Pack(Affine(saved->raw[index]),native);output[i]=native;
            const int side=saved->side[index];
            if(valid && side>=0) valid=ApplyPacked(native,delta[side],output[i]);
        }
        if(valid && Dirty(self)) {
            memcpy(self+0x30,output,count*48);InterlockedIncrement(&skinDraws);Record(1,self,saved->source,delta);
            if(saved->source.frame.twoHand) InterlockedIncrement(&supportedDraws);
            if(saved->source.frame.map) InterlockedIncrement(&mapDraws);
            if(saved->source.frame.phone) InterlockedIncrement(&phoneDraws);
        }
    }
    Unlock();
}
#include "arms_overlay.inl"
static void __fastcall Publish(uint8_t* gfx,void* edx) {
    originalPublish(gfx,edx);
    if(!installed || !writeGet || !gfx) return;
    const bool map=controls::MapOpen() && controls::MapControls() && !controls::PhoneOut() && !controls::VehicleMode();
    // Some calls keep normal controls and never assert PhoneOut. A directly
    // attached local hand item is still provable without guessing its model.
    const bool phone=controls::PhoneOut() && !controls::MapOpen() && !g_curWeapon;
    const bool gadget=!g_curWeapon && !controls::VehicleMode() && !controls::LoadingControls()
        && (controls::NormalControls() || phone);
    if(!map && !gadget && (gfx!=g_wGfx || !g_curWeapon)) return;
    if(IsBadReadPtr(gfx,0x1B0)) return;
    InterlockedIncrement(&worldSeen);
    auto ent=Entity(gfx);auto player=Entity((uint8_t*)g_playerGfx);
    if(!ent || !player || IsBadReadPtr(ent+0x78,4)) return;
    auto attachment=*(uint8_t**)(ent+0x78);
    if(!attachment || IsBadReadPtr(attachment,0xC0)) return;
    auto parentHandle=*(uint8_t**)(attachment+0x18);
    if(!parentHandle || IsBadReadPtr(parentHandle+12,4)) return;
    const bool direct=*(uint8_t**)(parentHandle+12)==player;
    if(!direct && !map) return;
    const int bone=*(int*)(attachment+0x1C);
    const auto hand=Affine(Read(attachment+0x40)),offset=Affine(Read(attachment+0x80)),world=Affine(Read(gfx+0x170));
    if(IsBadReadPtr(ent+0x30,64) || !Near(Multiply(offset,hand),world,0.0005f) || !Near(Affine(Read(ent+0x30)),world,0.0005f)) return;
    AttachmentSource nested{};
    if(!direct && !MapChildAttachment(ent,hand,nested)) return;
    auto parts=*(uint8_t***)(gfx+0x30);const unsigned count=*(unsigned*)(gfx+0x34);
    if(!parts || !count || count>32 || IsBadReadPtr(parts,count*4)) return;
    Lock();const Source* match=nullptr;bool ambiguous=false;int side=-1;
    if(!direct) { match=&nested.source;side=nested.side; }
    for(unsigned p=0;direct && p<History;++p) {
        const auto& s=poses[(poseNext+History-1-p)%History].source;
        const int candidateSide=bone==s.hand[1]?1:bone==s.hand[0]?0:-1;
        if(!s.body || s.frame.map!=map || s.frame.phone!=phone || s.frame.weapon!=g_curWeapon || candidateSide<0 || (!map && !gadget && candidateSide!=1)
            || s.graphics!=(uintptr_t)g_playerGfx || !Near(s.handWorld[candidateSide],hand,0.0005f)) continue;
        if(match && (side!=candidateSide || !Equivalent(*match,s))) { ambiguous=true;break; }
        if(!match) { match=&s;side=candidateSide; }
    }
    if(match && !ambiguous) {
        if(map || gadget) QueueGadgetDepth(gfx,ent);
        if(map && direct) RememberMapGraphics(ent,gfx);
        // Write-side resolver is used only on the native publication thread.
        for(unsigned i=0;i<count;++i) {
            auto part=parts[i];if(!part || IsBadReadPtr(part+0x34,0x4C)) continue;
            auto proxy=*(uint8_t**)(part+0x34);if(!proxy || IsBadReadPtr(proxy+0x1C,8)) continue;
            auto primitive=writeGet((void*)VA(0x10EEF898),nullptr,(uint64_t*)(proxy+0x1C));
            if(!primitive || IsBadReadPtr(primitive+0x70,8)) continue;
            const uint64_t handle=*(uint64_t*)(primitive+0x70);
            const auto resource=writeGet((void*)VA(0x10EEF7A0),nullptr,&handle);
            if(!resource || IsBadReadPtr(resource+0x10,0x4C)) continue;
            const auto raw=Affine(Read(resource+0x10));const auto fraction=Read3((float*)(resource+0x50));
            auto combined=raw;combined[12]+=fraction.x;combined[13]+=fraction.y;combined[14]+=fraction.z;
            if(!Near(combined,Multiply(Affine(Read(part+0x40)),world),0.0005f)) continue;
            // Display markers cannot evict the accepted direct attachment path.
            auto& w=direct ? worlds[worldNext++%Resources] : markerWorlds[markerWorldNext++%MarkerResources];
            w.source=*match;w.handle=handle;w.raw=raw;w.fraction=fraction;w.side=side;w.nested=!direct;
            InterlockedIncrement(&worldMatched);
            if(!direct) { const unsigned bit=MarkerBit(handle);markerMask[bit/64]|=uint64_t(1)<<(bit%64);InterlockedIncrement(&nestedMatched); }
        }
    }
    Unlock();
}
static void PresentWorld(uint8_t* resource,const uint64_t* handle,unsigned slot) {
    if(!resource || !handle || IsBadReadPtr(resource,0x5C)||IsBadReadPtr(handle,8)) return;
    const auto raw=Affine(Read(resource+0x10));const auto fraction=Read3((float*)(resource+0x50));
    Lock();const World* match=nullptr;bool owned=false,ambiguous=false;
    const unsigned bit=MarkerBit(*handle);
    const unsigned markers=(markerMask[bit/64]&(uint64_t(1)<<(bit%64))) ? (std::min)(markerWorldNext,MarkerResources) : 0;
    for(unsigned i=0;i<Resources+markers;++i) {
        const auto& w=i<Resources ? worlds[(worldNext+Resources-1-i)%Resources]
            : markerWorlds[(markerWorldNext+MarkerResources-1-(i-Resources))%MarkerResources];
        if(!w.source.body || w.handle!=*handle) continue;
        owned=true;
        if(Compatible(w.source) && !memcmp(w.raw.data(),raw.data(),64) && !memcmp(&w.fraction,&fraction,sizeof fraction)) {
            if(match && (match->side!=w.side || !Equivalent(match->source,w.source))) { ambiguous=true;break; }
            if(!match) match=&w;
        }
    }
    Matrix delta[3],worldDelta;
    const bool correct=match && !ambiguous && Deltas(match->source,delta,&worldDelta,match->side);
    if(owned) {
        const auto table=*(uint8_t**)resource;
        if(table && slot<4 && !IsBadReadPtr(table+4+slot*4,4)) {
            auto provider=*(uint8_t**)(table+4+slot*4);
            if(provider && !IsBadWritePtr(provider+0x30,64)) {
                auto corrected=raw;
                if(correct) {
                    auto relative=raw;
                    relative[12]+=fraction.x-match->source.root[12];relative[13]+=fraction.y-match->source.root[13];relative[14]+=fraction.z-match->source.root[14];
                    corrected=Multiply(relative,worldDelta);
                    corrected[12]+=match->source.root[12]-fraction.x;corrected[13]+=match->source.root[13]-fraction.y;corrected[14]+=match->source.root[14]-fraction.z;
                }
                if(detail::Affine(corrected) && Dirty(provider)) {
                    float transpose[16];for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) transpose[r*4+c]=corrected[c*4+r];
                    memcpy(provider+0x30,transpose,64);
                    if(correct) { InterlockedIncrement(&worldDraws);InterlockedIncrement(&worldHandDraws[match->side]);Record(2,provider,match->source,delta,match->side);if(match->nested) InterlockedIncrement(&nestedDraws);if(!match->source.frame.weapon && !match->source.frame.map) InterlockedIncrement(&gadgetDraws); }
                }
            }
        }
    }
    Unlock();
}
static uint8_t* __fastcall Get(void* pool,void* edx,const uint64_t* handle) {
    const auto caller=(uintptr_t)__builtin_return_address(0);
    auto resource=originalGet(pool,edx,handle);
    if(!installed || !renderThread || !resource || !handle) return resource;
    if(overlayInstalled && pool==(void*)VA(0x10EEF978) && caller==VA(0x103A386A)) return PresentOverlay(resource,handle);
    if(pool!=(void*)VA(0x10EEF7A0)) return resource;
    if(caller!=VA(0x103BAE26) && caller!=VA(0x103BB2D3) && caller!=VA(0x103B860F) && caller!=VA(0x103B989A)) return resource;
    PresentWorld(resource,handle,*(unsigned*)VA(0x10EEEF10));return resource;
}
} // namespace late

bool PresentationMapWorld(uint8_t* graphics,float world[16]) {
    using namespace late;
    if(!installed || !ready || !render.map || !graphics || !world || IsBadReadPtr(graphics+0x170,64)) return false;
    AttachmentSource owner;
    if(!MapAttachment(Entity(graphics),owner) || !Near(Affine(Read(graphics+0x170)),owner.world,0.0005f)) return false;
    Matrix delta[3],worldDelta;
    if(!Deltas(owner.source,delta,&worldDelta,owner.side,true)) return false;
    auto relative=owner.world;
    for(int k=0;k<3;++k) relative[12+k]-=owner.source.root[12+k];
    auto corrected=Multiply(relative,worldDelta);
    for(int k=0;k<3;++k) corrected[12+k]+=owner.source.root[12+k];
    if(!detail::Affine(corrected)) return false;
    memcpy(world,corrected.data(),64);return true;
}

static void RememberPresentation(uint8_t* skel,uint8_t* gfx,Bone* bones,int n,const M4& root,const float* origin,const Frame& frame,uint64_t bodySequence,const fc2vr::presentation::SupportHold& support) {
    using namespace late;
    if(!installed || n<=0 || n>int(MaxBones) || !frame.valid || !frame.handValid || !frame.leftValid || !frame.headValid
        || frame.bonePubSeat || !ModeAllowed(frame) || ReloadActive()
        || (frame.twoHand && support.mode==SupportMode::None)) return;
    Lock();auto& p=poses[poseNext++%History];p=Pose{};
    p.source.frame=frame;p.source.skeleton=(uintptr_t)skel;p.source.graphics=(uintptr_t)gfx;p.source.body=bodySequence;
    p.source.hand[0]=g_slot[20];p.source.hand[1]=g_slot[12];p.source.support=support;
    p.source.root=Read(root.m);
    p.source.frame.game.P=fc2vr::tracking::Subtract(frame.game.P,Read3(root.m[3]));
    for(unsigned k=0;k<3;++k) p.source.root[12+k]+=origin[k];
    p.count=(unsigned)n;p.workerBones=*(Bone**)(gfx+0x100);ownerBones=p.workerBones;
    int upper=g_slot[15];
    // Find the shared clavicle ancestor, then ascend the single-child spine to
    // its first branching parent. The pelvis and legs retain their foot planner.
    for(int guard=0;upper>=0 && upper<n && guard<n;++guard) {
        bool common=false;for(int j=g_slot[19],g=0;j>=0 && j<n && g<n;j=bones[j].parent,++g) if(j==upper) { common=true;break; }
        if(common) break;upper=bones[upper].parent;
    }
    for(int guard=0;upper>=0 && upper<n && guard<n;++guard) {
        const int parent=bones[upper].parent;if(parent<0 || parent>=n) break;
        int children=0;for(int b=0;b<n;++b) children+=bones[b].parent==parent;
        if(children!=1) break;upper=parent;
    }
    for(int i=0;i<n;++i) {
        p.models[i]=Affine(Read(bones[i].model));p.side[i]=-1;
        if(i==g_slot[0]) continue;
        const int mapped=g_follow[i]>=0?g_follow[i]:i;
        for(int j=mapped,g=0;j>=0 && j<n && g<n;j=bones[j].parent,++g) {
            if(j==g_slot[19]) { p.side[i]=0;break; }
            if(j==g_slot[15]) { p.side[i]=1;break; }
            if(j==upper) { p.side[i]=2;break; }
        }
    }
    for(int side=0;side<2;++side) if(p.source.hand[side]>=0 && p.source.hand[side]<n)
        p.source.handWorld[side]=Multiply(p.models[p.source.hand[side]],p.source.root);
    skinHandleCount=0;
    auto entries=*(uint8_t***)(gfx+0x120);const unsigned count=*(unsigned*)(gfx+0x124);
    if(entries && count<=64 && !IsBadReadPtr(entries,count*4)) for(unsigned i=0;i<count;++i) {
        auto entry=entries[i];if(entry && !IsBadReadPtr(entry+0xC,8)) skinHandles[skinHandleCount++]=*(uint64_t*)(entry+0xC);
    }
    Unlock();
}
void PreparePresentation(bool enabled) {
    late::eye=false;late::ready=false;late::renderThread=true;late::overlayViewNext=0;
    for(auto& c:late::cached) c=late::DeltaCache{};late::cacheNext=0;
    if(!late::installed || !enabled || ReloadActive()) return;
    Lock();late::render=g_frame;Unlock();const auto& f=late::render;
    late::ready=f.valid && f.handValid && f.leftValid && f.headValid && f.handAuthority && f.leftAuthority && f.headAuthority
        && f.bonePubValid && (f.bonePubRoot || f.map || f.phone) && !f.bonePubSeat && late::ModeAllowed(f);
    static ULONGLONG last=0;const auto now=GetTickCount64();
    if(now-last>5000) { last=now;LOG("interaction-21 late presentation: ready=%d twoHand=%d map=%d phone=%d worker=%ld/%ld skin=%ld supported=%ld mapSkin=%ld phoneSkin=%ld gadgetDraws=%ld worldSources=%ld/%ld worldDraws=%ld left=%ld right=%ld overlaySources=%ld/%ld overlayLeft=%ld overlayRight=%ld markerSources=%ld markerDraws=%ld screenDepth=%ld",late::ready,f.twoHand,f.map,f.phone,
        late::workerMatched,late::workerSeen,late::skinDraws,late::supportedDraws,late::mapDraws,late::phoneDraws,late::gadgetDraws,late::worldMatched,late::worldSeen,late::worldDraws,late::worldHandDraws[0],late::worldHandDraws[1],late::overlayMatched,late::overlaySeen,late::overlayDraws[0],late::overlayDraws[1],late::nestedMatched,late::nestedDraws,late::overlayDepthDraws); }
}
void BeginPresentationEye() { late::eye=late::ready; }
void EndPresentationEye() { late::eye=false; }
static void InstallPresentation(PatchFn patch) {
    if(gameaddr::Detect()!=gameaddr::GOG || !g_c.weaponInHand) return;
    const uint8_t pro6[]={0x55,0x8B,0xEC,0x83,0xE4,0xF0};
    const uint8_t cache7[]={0x51,0x53,0x55,0x8B,0x6C,0x24,0x10};
    const uint8_t get7[]={0x8B,0x41,0x0C,0x8B,0x4C,0x24,0x04};
    // Preflight every seam before the first patch. Partial install stays inert.
    if(memcmp((void*)late::VA(0x10611660),pro6,6)||memcmp((void*)late::VA(0x103DFA70),pro6,6)
        ||memcmp((void*)late::VA(0x1050D300),pro6,6)||memcmp((void*)late::VA(0x103A5A90),cache7,7)
        ||memcmp((void*)late::VA(0x1043B920),get7,7)) { LOG("onfoot-15 late presentation: unsupported native entry");return; }
    late::writeGet=(late::Get_t)late::VA(0x1043B7E0);
    const bool get=patch(late::VA(0x1043B920),get7,7,(void*)&late::Get,(void**)&late::originalGet);
    const bool worker=patch(late::VA(0x10611660),pro6,6,(void*)&late::Worker,(void**)&late::originalWorker);
    const bool fill=patch(late::VA(0x103DFA70),pro6,6,(void*)&late::Fill,(void**)&late::originalFill);
    const bool cache=patch(late::VA(0x103A5A90),cache7,7,(void*)&late::Cache,(void**)&late::originalCache);
    const bool publish=patch(late::VA(0x1050D300),pro6,6,(void*)&late::Publish,(void**)&late::originalPublish);
    late::installed=get && worker && fill && cache && publish;
    LOG("onfoot-15 late presentation hooks: get=%d worker=%d fill=%d cache=%d publish=%d enabled=%d",get,worker,fill,cache,publish,late::installed);
    const uint8_t compass5[]={0x51,0x53,0x55,0x56,0x57};
    const uint8_t displayGet5[]={0xE8,0xB6,0x80,0x09,0x00};
    if(late::installed && !memcmp((void*)late::VA(0x1008A520),compass5,5)
        && !memcmp((void*)late::VA(0x103A3865),displayGet5,5))
        late::overlayInstalled=patch(late::VA(0x1008A520),compass5,5,(void*)&late::CompassUpdate,(void**)&late::originalCompassUpdate);
    LOG("overlay-18 compass display hook: enabled=%d",late::overlayInstalled);
    const uint8_t nearCommand[]={0x8b,0x41,0x6c,0x8d,0x14,0xc5,0,0,0,0,0x2b,0xd0,
        0x8b,0x41,0x68,0x8b,0x4c,0x24,0x04,0x89,0x4c,0xd0,0xe0,0xc2,0x04,0};
    const uint8_t specialView[]={0x74,0x0e,0x8b,0x4d,0x28,0x83,0x79,0x04,0,
        0xc6,0x44,0x24,0x0f,1,0x75,0x05,0xc6,0x44,0x24,0x0f,0};
    const uint8_t nearPass[]={0x80,0x7d,0x34,0,0x89,0x4c,0x24,0x1c,0x74,0x07,0x83,0xc9,4,
        0x89,0x4c,0x24,0x1c,0x80,0x7c,0x24,0x0f,0,0x75,0x22};
    late::overlayDepthVerified=late::overlayInstalled
        && !memcmp((void*)late::VA(0x1044DFC0),nearCommand,sizeof nearCommand)
        && !memcmp((void*)late::VA(0x103A205E),specialView,sizeof specialView)
        && !memcmp((void*)late::VA(0x103A2C77),nearPass,sizeof nearPass);
    LOG("interaction-21 screen world-depth route: verified=%d",late::overlayDepthVerified);
}
