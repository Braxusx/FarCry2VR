// Included inside late. GOG CCompassObjectives queues display tiles separately
// from the map/GPS meshes. See docs/PRESENTATION.md for the native trace.
constexpr unsigned OverlayHistory=64, OverlayMatrices=256, OverlayViews=32;
struct AttachmentSource { Source source{}; Matrix world{}; uintptr_t entity=0; int side=-1; };
struct Overlay {
    AttachmentSource owner{}; uint64_t handle=0; unsigned count=0;
    Matrix raw[OverlayMatrices]{};
};
struct alignas(16) OverlayView {
    uint8_t resource[0xA0]{}; Matrix matrices[OverlayMatrices]{};
    uint8_t commands[256*0x38]{};
};
static Overlay overlays[OverlayHistory]; static unsigned overlayNext=0;
static thread_local OverlayView overlayViews[OverlayViews];
static thread_local unsigned overlayViewNext=0;
static volatile LONG overlaySeen=0,overlayMatched=0,overlayDraws[2]{};
using CompassUpdate_t=void (__fastcall *)(uint8_t*,void*,float,uint32_t);
static CompassUpdate_t originalCompassUpdate=nullptr;
static bool overlayInstalled=false;
static bool overlayDepthVerified=false;
static volatile LONG overlayDepthDraws=0;
static volatile LONG nestedMatched=0,nestedDraws=0;
// Negative lookup filter: unrelated world draws do not scan marker history.
// Bits only accumulate; false positives still require exact full-handle matches.
static uint64_t markerMask[64]{};
static unsigned MarkerBit(uint64_t handle) { return unsigned(handle^(handle>>32))&4095u; }
struct MapGraphics { uint8_t* entity=nullptr;uint8_t* graphics=nullptr; };
static MapGraphics mapGraphics[8];static unsigned mapGraphicsNext=0;
static void RememberMapGraphics(uint8_t* entity,uint8_t* graphics) { // g_cs held
    for(auto& entry:mapGraphics) if(entry.entity==entity) { entry.graphics=graphics;return; }
    mapGraphics[mapGraphicsNext++%8]={entity,graphics};
}

// Match the owning entity to the exact hand transform consumed by the native
// attachment. The display matrices must independently match this owner and its
// screen bone after the native update has finished synchronizing pending work.
static bool MapAttachment(uint8_t* ent,AttachmentSource& out) {
    if(!ent || IsBadReadPtr(ent+0x30,64) || IsBadReadPtr(ent+0x78,4)) return false;
    auto player=Entity((uint8_t*)g_playerGfx),at=*(uint8_t**)(ent+0x78);
    if(!player || !at || IsBadReadPtr(at,0xC0)) return false;
    auto parent=*(uint8_t**)(at+0x18);
    if(!parent || IsBadReadPtr(parent+12,4) || *(uint8_t**)(parent+12)!=player) return false;
    const int bone=*(int*)(at+0x1C);
    const auto hand=Affine(Read(at+0x40)),world=Affine(Read(ent+0x30));
    if(!Near(Multiply(Affine(Read(at+0x80)),hand),world,0.0005f)) return false;
    Lock();const Source* match=nullptr;int side=-1;bool ambiguous=false;
    for(unsigned i=0;i<History;++i) {
        const auto& s=poses[(poseNext+History-1-i)%History].source;
        const int which=bone==s.hand[0]?0:bone==s.hand[1]?1:-1;
        if(!s.body || !s.frame.map || s.frame.twoHand || s.frame.weapon!=g_curWeapon || which<0
            || s.graphics!=(uintptr_t)g_playerGfx || !Near(s.handWorld[which],hand,0.0005f)) continue;
        if(match && (side!=which || !Equivalent(*match,s))) { ambiguous=true;break; }
        if(!match) { match=&s;side=which; }
    }
    if(match && !ambiguous) { out.source=*match;out.world=world;out.entity=(uintptr_t)ent;out.side=side; }
    Unlock();return match && !ambiguous;
}
static bool MapChildAttachment(uint8_t* ent,const Matrix& anchor,AttachmentSource& out) {
    if(!ent || IsBadReadPtr(ent+0x78,4)) return false;
    auto at=*(uint8_t**)(ent+0x78);
    if(!at || IsBadReadPtr(at,0xC0)) return false;
    auto handle=*(uint8_t**)(at+0x18);
    if(!handle || IsBadReadPtr(handle+12,4)) return false;
    auto parent=*(uint8_t**)(handle+12);
    if(!MapAttachment(parent,out)) return false;
    // Exactly one additional attachment level: display markers attached to a
    // held map/GPS bone. Never walk the objective's world-target entity list.
    const int bone=*(int*)(at+0x1C);auto expected=out.world;
    if(bone!=-1) {
        auto parentAt=*(uint8_t**)(parent+0x78);
        if(bone<0 || IsBadReadPtr(parentAt+0xC0,4)) return false;
        auto container=*(uint8_t**)(parentAt+0xC0);
        if(!container || IsBadReadPtr(container+0x20,16)) return false;
        auto bones=*(Bone**)(container+0x20);const unsigned count=*(unsigned*)(container+0x2C);
        if(!bones || count>MaxBones || unsigned(bone)>=count || IsBadReadPtr(bones,count*sizeof(Bone))) return false;
        expected=Multiply(Affine(Read(bones[bone].model)),out.world);
    }
    return Near(expected,anchor,0.0005f);
}
static uint8_t* CompassEntity(uint8_t* secondary) {
    if(!secondary || IsBadReadPtr(secondary+4,4)) return nullptr;
    auto handle=*(uint8_t**)(secondary+4);
    return handle && !IsBadReadPtr(handle+12,4) ? *(uint8_t**)(handle+12) : nullptr;
}
static bool DisplayBone(const AttachmentSource& owner,int bone,Matrix& expected) {
    expected=owner.world;if(bone==-1) return true;
    if(bone<0) return false;
    auto ent=(uint8_t*)owner.entity;
    if(!ent || IsBadReadPtr(ent+0x78,4)) return false;
    auto at=*(uint8_t**)(ent+0x78);uint8_t* container=nullptr;
    if(at && !IsBadReadPtr(at+0xC0,4)) container=*(uint8_t**)(at+0xC0);
    // The native attachment populates its skeleton cache when it has children.
    // A display without marker children may not have that cache yet. A proven
    // publication supplies the same graphics container, without native queries.
    if(!container) {
        Lock();
        for(const auto& entry:mapGraphics) if(entry.entity==ent && Entity(entry.graphics)==ent
            && !IsBadReadPtr(entry.graphics+0x170,64) && Near(Affine(Read(entry.graphics+0x170)),owner.world,0.0005f)) {
            container=entry.graphics+0xE0;break;
        }
        Unlock();
    }
    if(!container || IsBadReadPtr(container+0x20,16)) return false;
    auto bones=*(Bone**)(container+0x20);const unsigned count=*(unsigned*)(container+0x2C);
    if(!bones || count>MaxBones || unsigned(bone)>=count || IsBadReadPtr(bones,count*sizeof(Bone))) return false;
    expected=Multiply(Affine(Read(bones[bone].model)),owner.world);return detail::Affine(expected);
}
static bool OverlayData(uint8_t* resource,const Matrix*& matrices,unsigned& count) {
    if(!resource || IsBadReadPtr(resource,0xA0)) return false;
    matrices=*(Matrix**)(resource+0x5C);count=*(unsigned*)(resource+0x60);
    if(!matrices || count<2 || count>OverlayMatrices || IsBadReadPtr(matrices,count*64)) return false;
    auto commands=*(uint8_t**)(resource+0x68);const unsigned commandsCount=*(unsigned*)(resource+0x6C);
    if(!commands || !commandsCount || commandsCount>256 || IsBadReadPtr(commands,commandsCount*0x38)) return false;
    // This display path uses explicit world transforms; index zero is the
    // resource's default matrix. Unknown/default-matrix commands stay native.
    for(unsigned i=0;i<commandsCount;++i) {
        const unsigned index=*(unsigned*)(commands+i*0x38+0x28);
        if(!index || index>=count) return false;
    }
    for(unsigned i=0;i<count;++i) if(!detail::Affine(matrices[i])) return false;
    return true;
}
static void RememberOverlay(uint8_t* resource,uint64_t handle,const AttachmentSource& owner,const Matrix& expected) {
    const Matrix* matrices;unsigned count;
    if(!OverlayData(resource,matrices,count)) return;
    // Native 100893E0 copies this boneModel * ownerWorld for every tile. Prove
    // which completed pose actually produced the queue instead of guessing
    // from its age or requiring the owner to stay still across a native wait.
    for(unsigned i=1;i<count;++i) if(!Near(matrices[i],expected,0.0005f)) return;
    Lock();auto& saved=overlays[overlayNext++%OverlayHistory];
    saved.owner=owner;saved.handle=handle;saved.count=count;memcpy(saved.raw,matrices,count*64);
    InterlockedIncrement(&overlayMatched);Unlock();
}
static void __fastcall CompassUpdate(uint8_t* self,void* edx,float dt,uint32_t opaque) {
    originalCompassUpdate(self,edx,dt,opaque);
    const bool watch=installed && writeGet && controls::MapOpen() && controls::MapControls()
        && !controls::PhoneOut() && !controls::VehicleMode() && !controls::LoadingControls()
        && self && !IsBadReadPtr(self+0x2C,4) && !IsBadReadPtr(self+0xA8,8);
    if(!watch) return;
    InterlockedIncrement(&overlaySeen);AttachmentSource owner{};Matrix expected;
    if(!MapAttachment(CompassEntity(self),owner) || !DisplayBone(owner,*(int*)(self+0x2C),expected)) return;
    const auto handles=*(uint64_t**)(self+0xA8);const unsigned count=*(unsigned*)(self+0xAC);
    if(!handles || count!=4 || IsBadReadPtr(handles,count*8)) return;
    for(unsigned i=0;i<count;++i) {
        const auto handle=handles[i];
        // Pure write-bank lookup, not the allocating/COW resolver 1044AA40.
        auto resource=writeGet((void*)VA(0x10EEF978),nullptr,&handle);
        RememberOverlay(resource,handle,owner,expected);
    }
}
static void WorldOverlayDepth(OverlayView& view,const uint8_t* resource) {
    if(!overlayDepthVerified || !g_nearViewport) return;
    // Only the known CCompassObjectives world-tile recipe. Keep late motion
    // correction even when an unfamiliar recipe cannot use this depth route.
    if(resource[0x8E] || *(const unsigned*)(resource+0x90)!=1
        || *(const unsigned*)(resource+0x94)!=1) return;
    const auto commands=*(uint8_t* const*)(resource+0x68);
    const unsigned count=*(const unsigned*)(resource+0x6C); // OverlayData checked
    for(unsigned i=0;i<count;++i) {
        const auto c=commands+i*0x38;
        if(*(const unsigned*)c!=0 || *(const unsigned*)(c+4)!=0x81
            || *(const unsigned*)(c+8)!=0) return;
    }
    memcpy(view.commands,commands,count*0x38);
    unsigned nearMask=0;
    for(unsigned i=0;i<count;++i) {
        auto& mask=*(unsigned*)(view.commands+i*0x38+0x18);
        nearMask|=mask;mask=0;
    }
    // 103A205E/103A2C88: bypass the special view-mask submission and use the
    // regular 3D tile queue. 103A2C77: a command +18 mask would otherwise select
    // OpaqueNearZ (pass 4), so clear it in the copy as well. The result is the
    // native Opaque world pass (0), with its normal per-pixel depth test.
    // Native buffers, textures, vertices, state flags and geometry stay intact.
    *(unsigned*)(view.resource+0x94)=0;
    *(uint8_t**)(view.resource+0x68)=view.commands;
    const LONG draws=InterlockedIncrement(&overlayDepthDraws);
    if(draws<=4) LOG("interaction-21 screen depth: commands=%u nearMask=%08x specialViewMask=1 -> world opaque",count,nearMask);
}
static uint8_t* PresentOverlay(uint8_t* resource,const uint64_t* handle) {
    if(!eye || !render.map || !handle || IsBadReadPtr(handle,8) || overlayViewNext>=OverlayViews) return resource;
    const Matrix* matrices;unsigned count;
    if(!OverlayData(resource,matrices,count)) return resource;
    Lock();const Overlay* match=nullptr;bool ambiguous=false;
    for(unsigned i=0;i<OverlayHistory;++i) {
        const auto& s=overlays[(overlayNext+OverlayHistory-1-i)%OverlayHistory];
        if(s.handle!=*handle || s.count!=count || !Compatible(s.owner.source)
            || memcmp(s.raw,matrices,count*64)) continue;
        if(match && (match->owner.entity!=s.owner.entity || match->owner.side!=s.owner.side
            || !Equivalent(match->owner.source,s.owner.source))) { ambiguous=true;break; }
        if(!match) match=&s;
    }
    Matrix delta[3],worldDelta;
    if(match && !ambiguous && Deltas(match->owner.source,delta,&worldDelta,match->owner.side)) {
        auto& view=overlayViews[overlayViewNext];bool valid=true;
        view.matrices[0]=matrices[0];
        for(unsigned i=1;i<count;++i) {
            auto relative=matrices[i];
            for(unsigned k=0;k<3;++k) relative[12+k]-=match->owner.source.root[12+k];
            auto corrected=Multiply(relative,worldDelta);
            for(unsigned k=0;k<3;++k) corrected[12+k]+=match->owner.source.root[12+k];
            valid=valid && detail::Affine(corrected);view.matrices[i]=corrected;
        }
        if(valid) {
            memcpy(view.resource,resource,sizeof view.resource);
            *(Matrix**)(view.resource+0x5C)=view.matrices;
            WorldOverlayDepth(view,resource);
            ++overlayViewNext;InterlockedIncrement(&overlayDraws[match->owner.side]);
            Record(3,view.resource,match->owner.source,delta,match->owner.side);
            Unlock();return view.resource;
        }
    }
    Unlock();return resource;
}
