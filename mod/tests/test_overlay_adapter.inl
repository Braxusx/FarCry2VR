// Production display queue/update/get bodies with owned native-layout buffers.
static std::array<uint8_t,0x110> compass{};
static std::array<uint64_t,4> displayHandles{{0x1100000011ull,0x2200000022ull,0x3300000033ull,0x4400000044ull}};
static std::array<std::array<uint8_t,0xA0>,4> displayResources{};
static std::array<std::array<Matrix,3>,4> displayMatrices{};
static std::array<uint8_t,0x70> displayCommands{};
static std::array<uint8_t,0x30> displayContainer{};
static std::array<arms::Bone,1> displayBones{};
static int compassCalls=0;static float compassDt=0;static uint32_t compassOpaque=0;
static bool compassChangesOwner=false;
static bool compassCompletesOwner=false;
static void __fastcall NativeCompass(uint8_t*,void*,float dt,uint32_t opaque) {
    ++compassCalls;compassDt=dt;compassOpaque=opaque;
    if(compassChangesOwner) { auto world=adapter::Read(childEntity.data()+0x30);world[12]+=.1f;Store(childEntity,0x30,world); }
    if(compassCompletesOwner) Store(childEntity,0x30,Multiply(adapter::Read(attachment.data()+0x80),adapter::Read(attachment.data()+0x40)));
}
static uint8_t* __fastcall OverlayWriteGet(void* pool,void* edx,const uint64_t* handle) {
    if(pool==(void*)adapter::VA(0x10EEF978)) {
        for(unsigned i=0;i<4;++i) if(*handle==displayHandles[i]) return displayResources[i].data();
        return nullptr;
    }
    return WriteGet(pool,edx,handle);
}
static void PrepareOverlay(int side) {
    PrepareAttachment(side,true);compass.fill(0);displayCommands.fill(0);
    compassCalls=0;compassDt=0;compassOpaque=0;compassChangesOwner=compassCompletesOwner=false;
    Store(compass,4,childHandle.data());Store(compass,0xA8,displayHandles.data());Store(compass,0xAC,4u);
    Store(compass,0x2C,0);displayContainer.fill(0);displayBones[0]={};
    const auto displayBone=Model(0,.02f,.03f);std::memcpy(displayBones[0].model,displayBone.data(),64);
    Store(displayContainer,0x20,displayBones.data());Store(displayContainer,0x2C,1u);Store(attachment,0xC0,displayContainer.data());
    Store(displayCommands,0x28,1u);Store(displayCommands,0x38+0x28,2u);
    for(unsigned i=0;i<2;++i) {
        Store(displayCommands,i*0x38+4,0x81u);Store(displayCommands,i*0x38+0x18,1u<<i);
        Store(displayCommands,i*0x38+0x1C,0xAABBCCDDu+i); // opaque native texture identity
    }
    const auto world=adapter::Read(childEntity.data()+0x30);
    for(unsigned i=0;i<4;++i) {
        displayResources[i].fill(0);displayMatrices[i][0]=Identity();
        displayMatrices[i][1]=displayMatrices[i][2]=Multiply(displayBone,world);
        Store(displayResources[i],0x10,Identity());Store(displayResources[i],0x5C,displayMatrices[i].data());
        Store(displayResources[i],0x60,3u);Store(displayResources[i],0x68,displayCommands.data());Store(displayResources[i],0x6C,2u);
        Store(displayResources[i],0x90,1u);Store(displayResources[i],0x94,1u);
    }
    adapter::writeGet=OverlayWriteGet;adapter::originalCompassUpdate=NativeCompass;
}
static void RecordOverlay() { adapter::CompassUpdate(compass.data(),nullptr,.0125f,0xAABBCCDDu); }
static void OverlayDisplays() {
    for(int side=0;side<2;++side) {
        PrepareOverlay(side);const auto nativeResources=displayResources;const auto nativeMatrices=displayMatrices;
        const auto nativeCommands=displayCommands;const auto nativeEntity=childEntity;const auto nativeAttachment=attachment;
        RecordOverlay();
        Check(compassCalls==1 && compassDt==.0125f && compassOpaque==0xAABBCCDDu,"compass trampoline forwards both stack args exactly once");
        Check(adapter::overlayMatched==4,"all four owned display queue handles are recorded");
        Target();arms::BeginPresentationEye();
        auto* view=adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0]);
        Check(view!=displayResources[0].data(),"display uses a render-local resource copy");
        const auto first=*reinterpret_cast<Matrix**>(view+0x5C);
        auto expected=nativeMatrices[0][1];expected[12]+=side?.09f:.01f;if(!side) expected[13]-=.06f;
        Check(adapter::Near(first[1],expected,.00004f),"display transforms use same hand delta as housing");
        Check(first[0]==nativeMatrices[0][0],"unused default matrix remains native");
        const auto firstMatrix=first[1];
        auto* repeat=adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0]);
        Check(adapter::Near((*reinterpret_cast<Matrix**>(repeat+0x5C))[1],firstMatrix),"repeated display reads never accumulate correction");
        arms::EndPresentationEye();arms::BeginPresentationEye();
        auto* second=adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0]);
        Check(adapter::Near((*reinterpret_cast<Matrix**>(second+0x5C))[1],firstMatrix),"both eyes share the frozen display correction");
        auto resourceCopy=nativeResources[0];Store(resourceCopy,0x5C,*reinterpret_cast<Matrix**>(second+0x5C));
        auto commands=*reinterpret_cast<uint8_t**>(second+0x68);
        Store(resourceCopy,0x68,commands);Store(resourceCopy,0x94,0u);
        Check(!std::memcmp(resourceCopy.data(),second,resourceCopy.size()),"resource copy changes only matrix/command pointers and special view mask");
        auto expectedCommands=nativeCommands;Store(expectedCommands,0x18,0u);Store(expectedCommands,0x38+0x18,0u);
        Check(commands!=displayCommands.data() && !std::memcmp(commands,expectedCommands.data(),expectedCommands.size()),"render-local commands clear only foreground masks and preserve every other byte");
        Check(adapter::overlayDepthDraws==3,"repeated reads and both eyes use world-depth tile routing");
        Check(*reinterpret_cast<uint8_t**>(view+0x68)!=commands,"both eyes retain separate command copies until frame completion");
        Check(displayResources==nativeResources && displayMatrices==nativeMatrices && displayCommands==nativeCommands
            && childEntity==nativeEntity && attachment==nativeAttachment,"display correction preserves every native queue/owner byte");
        arms::EndPresentationEye();
        Check(adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0])==displayResources[0].data(),"non-eye display reads always return native resource");
        Check(lockDepth==0,"display adapter releases all locks");
    }
    // Compare independent production paths under physical and stick rotation:
    // a display's local offset must remain fixed against its corrected housing.
    for(int side=0;side<2;++side) for(int turn=0;turn<2;++turn) {
        PrepareOverlay(side);adapter::Publish(childGfx.data(),nullptr);RecordOverlay();Target();
        constexpr float c=.965925826f,s=.258819045f;
        if(turn==0) {
            auto& f=arms::g_frame;Put(side?f.aimR:f.laimR,{c,0,-s});Put(side?f.aimB:f.laimB,{s,0,c});
        } else { arms::g_frame.game.R={c,s,0};arms::g_frame.game.F={-s,c,0}; }
        arms::PreparePresentation(true);arms::BeginPresentationEye();
        adapter::PresentWorld(worldResource.data(),&WorldHandle,0);auto housing=ProviderWorld();
        const auto fraction=arms::Read3(reinterpret_cast<float*>(worldResource.data()+0x50));
        housing[12]+=fraction.x;housing[13]+=fraction.y;housing[14]+=fraction.z;
        auto* view=adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0]);
        Check(view!=displayResources[0].data(),"rotating display has a proven render copy");
        const auto display=(*reinterpret_cast<Matrix**>(view+0x5C))[1];
        Check(adapter::Near(display,Multiply(Model(0,.02f,.03f),housing),.00005f),"display stays rigidly seated during physical or stick turning");
        arms::EndPresentationEye();
    }
}
static void OverlayRejection() {
    for(int unsupported=0;unsupported<8;++unsupported) {
        PrepareOverlay(1);
        if(unsupported==0) adapter::overlayDepthVerified=false;
        if(unsupported==1) arms::g_nearViewport=false;
        if(unsupported==2) displayResources[0][0x8E]=1;
        if(unsupported==3) Store(displayResources[0],0x90,2u);
        if(unsupported==4) Store(displayResources[0],0x94,2u);
        if(unsupported==5) Store(displayCommands,0x38,1u);
        if(unsupported==6) Store(displayCommands,0x38+4,0x89u);
        if(unsupported==7) Store(displayCommands,0x38+8,1u);
        const auto nativeResource=displayResources[0];const auto nativeCommands=displayCommands;
        RecordOverlay();Target();arms::BeginPresentationEye();
        auto* view=adapter::PresentOverlay(displayResources[0].data(),&displayHandles[0]);
        Check(view!=displayResources[0].data(),"unsupported depth recipe preserves accepted motion correction");
        auto expected=nativeResource;Store(expected,0x5C,*reinterpret_cast<Matrix**>(view+0x5C));
        Check(!std::memcmp(view,expected.data(),expected.size()) && adapter::overlayDepthDraws==0,"unknown recipe or unavailable depth route leaves all depth fields native");
        Check(displayCommands==nativeCommands && displayResources[0]==nativeResource,"depth fallback never partially edits a native queue");
        arms::EndPresentationEye();
    }
    for(int invalid=0;invalid<12;++invalid) {
        PrepareOverlay(0);RecordOverlay();Target();arms::BeginPresentationEye();auto handle=displayHandles[0];
        if(invalid==0) handle+=0x100000000ull;
        if(invalid==1) displayMatrices[0][1][12]+=.01f;
        if(invalid==2) Store(displayCommands,0x28,3u);
        if(invalid==3) Store(displayCommands,0x28,0u);
        if(invalid==4) Store(displayResources[0],0x60,adapter::OverlayMatrices+1);
        if(invalid==5) displayMatrices[0][1][0]=std::numeric_limits<float>::quiet_NaN();
        if(invalid==6) adapter::render.roomEpoch++;
        if(invalid==7) adapter::render.map=false;
        if(invalid==8) clockMs+=1000;
        if(invalid==9) adapter::overlayViewNext=adapter::OverlayViews;
        if(invalid==10) {
            adapter::overlays[4]=adapter::overlays[0];adapter::overlays[4].owner.source.frame.lgripO[0]+=.01f;adapter::overlayNext=5;
        }
        if(invalid==11) adapter::render.source.handGeneration[0]++;
        Check(adapter::PresentOverlay(displayResources[0].data(),&handle)==displayResources[0].data(),"invalid/stale/ambiguous display retains native resource");
        Check(adapter::overlayDraws[0]==0 && lockDepth==0,"display rejection is atomic and releases lock");
        arms::EndPresentationEye();
    }
    for(int invalid=0;invalid<5;++invalid) {
        PrepareOverlay(1);
        if(invalid==0) compassChangesOwner=true;
        if(invalid==1) Store(attachment,0x18,childHandle.data());
        if(invalid==2) Store(compass,0xAC,5u);
        if(invalid==3) mapControls=false;
        if(invalid==4) Store(attachment,0x1C,20);
        RecordOverlay();Check(compassCalls==1 && adapter::overlayMatched==0,"unproven or changing display owner cannot stamp resources");
    }
    PrepareOverlay(0);compassCompletesOwner=true;Store(childEntity,0x30,Identity());RecordOverlay();
    Check(adapter::overlayMatched==4,"native synchronization may finish owner pose before display provenance is checked");
    PrepareOverlay(1);Store(attachment,0xC0,static_cast<uint8_t*>(nullptr));
    Store(childGfx,0x100,displayBones.data());Store(childGfx,0x10C,1u);
    adapter::Publish(childGfx.data(),nullptr);RecordOverlay();
    Check(adapter::overlayMatched==4,"proven graphics publication supplies screen bone when attachment cache is absent");
    PrepareOverlay(1);Store(attachment,0xC0,static_cast<uint8_t*>(nullptr));RecordOverlay();
    Check(adapter::overlayMatched==0,"missing screen skeleton never substitutes a guessed transform");
    PrepareOverlay(0);displayMatrices[0][1][12]+=.01f;RecordOverlay();
    Check(adapter::overlayMatched==3,"queued matrices must prove the exact owner and screen bone");
    PrepareOverlay(0);Store(compass,0x2C,-1);
    const auto ownerWorld=adapter::Read(childEntity.data()+0x30);
    for(auto& matrices:displayMatrices) matrices[1]=matrices[2]=ownerWorld;
    RecordOverlay();Check(adapter::overlayMatched==4,"native no-bone display uses the proven owner world directly");
}
static void MapCompositorAlignment() {
    for(int side=0;side<2;++side) {
        PrepareAttachment(side,true);
        adapter::Publish(childGfx.data(),nullptr);Target();
        Matrix actual=Identity();const auto nativeGraphics=childGfx;
        Check(!adapter::eye,"compositor query starts outside an eye draw");
        Check(arms::PresentationMapWorld(childGfx.data(),actual.data()),"map HUD accepts the exact held attachment");
        arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);arms::EndPresentationEye();
        auto rendered=ProviderWorld();const auto fraction=arms::Read3((float*)(worldResource.data()+0x50));
        rendered[12]+=fraction.x;rendered[13]+=fraction.y;rendered[14]+=fraction.z;
        Check(adapter::Near(actual,rendered,.00004f),"map HUD and rendered surface use the same transform");
        Check(childGfx==nativeGraphics,"map HUD leaves native graphics bytes untouched");
        Check(!adapter::eye,"map HUD leaves eye ownership unchanged");
        const auto accepted=actual;
        Store(childGfx,0x170,Identity());
        Check(!arms::PresentationMapWorld(childGfx.data(),actual.data()) && actual==accepted,"unmatched map leaves HUD output untouched");
        childGfx=nativeGraphics;adapter::render.source.generation++;
        Check(!arms::PresentationMapWorld(childGfx.data(),actual.data()) && actual==accepted,"recenter rejects old map HUD history");
        adapter::ready=false;
        Check(!arms::PresentationMapWorld(childGfx.data(),actual.data()),"disabled presentation cannot correct map HUD");
    }
}
static void NestedMapMarkers() {
    std::array<uint8_t,0x80> markerEntity{};std::array<uint8_t,16> markerHandle{};
    std::array<uint8_t,0xC0> markerAt{};std::array<uint8_t,0xC4> parentAt{};
    std::array<uint8_t,0x30> container{};std::array<arms::Bone,1> markerBones{};
    for(int invalid=-1;invalid<6;++invalid) {
        PrepareAttachment(0,true);markerEntity.fill(0);markerHandle.fill(0);markerAt.fill(0);parentAt.fill(0);container.fill(0);
        std::memcpy(parentAt.data(),attachment.data(),attachment.size());
        Store(childEntity,0x78,parentAt.data());Store(parentAt,0xC0,container.data());
        Store(container,0x20,markerBones.data());Store(container,0x2C,1u);
        const auto bone=Model(.02f,.03f,.04f);std::memcpy(markerBones[0].model,bone.data(),64);
        const auto anchor=Multiply(bone,adapter::Read(childEntity.data()+0x30)),world=Multiply(Model(.01f,0,0),anchor);
        Store(markerHandle,12,markerEntity.data());Store(markerEntity,0x78,markerAt.data());Store(markerEntity,0x30,world);
        Store(markerAt,0x18,childHandle.data());Store(markerAt,0x1C,0);Store(markerAt,0x40,anchor);Store(markerAt,0x80,Model(.01f,0,0));
        Store(childGfx,8,markerHandle.data());Store(childGfx,0x170,world);Store(worldResource,0x10,world);Store(worldResource,0x50,Vec3{});
        if(invalid==0) Store(parentAt,0xC0,static_cast<uint8_t*>(nullptr));
        if(invalid==1) Store(markerAt,0x1C,1);
        if(invalid==2) markerBones[0].model[12]+=.01f;
        if(invalid==3) Store(parentAt,0x18,childHandle.data());
        if(invalid==4) Store(markerAt,0x40,Identity());
        if(invalid==5) Store(childGfx,0x170,Identity());
        adapter::Publish(childGfx.data(),nullptr);
        Check(publishCalls==1,"nested marker publication calls native exactly once");
        if(invalid<0) {
            Check(adapter::nestedMatched==1 && adapter::markerWorlds[0].side==0,"marker inherits exact map hand through one proven parent");
            const auto nativeAt=markerAt;const auto nativeEntity=markerEntity;const auto nativeWorld=worldResource;
            Target();arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);arms::EndPresentationEye();
            auto expected=world;expected[12]+=.01f;expected[13]-=.06f;
            Check(adapter::Near(ProviderWorld(),expected,.00004f),"nested marker stays with corrected map surface");
            Check(markerAt==nativeAt && markerEntity==nativeEntity && worldResource==nativeWorld,"nested marker native transforms remain untouched");
        } else Check(adapter::worldMatched==0,"unproven nested marker falls back to native rendering");
    }
    const auto raw=PrepareAttachment(0,true);adapter::Publish(childGfx.data(),nullptr);
    const auto direct=adapter::worlds[0];
    for(unsigned i=0;i<adapter::MarkerResources+40;++i) {
        auto& marker=adapter::markerWorlds[adapter::markerWorldNext++%adapter::MarkerResources];
        marker=direct;marker.handle=WorldHandle+1+i;marker.nested=true;
    }
    Target();arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);arms::EndPresentationEye();
    auto expected=raw;expected[12]+=.01f;expected[13]-=.06f;
    Check(adapter::Near(ProviderWorld(),expected,.00004f),"hundreds of marker publications cannot evict a direct map/GPS source");
}
