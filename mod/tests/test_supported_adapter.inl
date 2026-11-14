// Included inside the existing Win32 native-adapter harness.
static void SupportedAndMapSkin() {
    for(int mode=0;mode<3;++mode) {
        Reset();auto& source=originalSource;
        if(mode==0) {
            normal=false;mapOpen=mapControls=true;source.frame.map=true;
            source.frame.bonePubRoot=false; // map uses the matched native camera anchor
        } else {
            source.frame.twoHand=true;
            source.support={mode==1?SupportMode::Rifle:SupportMode::Pistol,{}, {-.6f,0,0}};
        }
        adapter::poses[0].source=source;Capture();Target();
        arms::BeginPresentationEye();const auto one=Draw();
        Matrix d[3];Check(adapter::Deltas(source,d),"map and supported holds compute valid production deltas");
        if(mode==1) Check(adapter::Near(d[0],d[1]),"rifle left/right chains share exact correction");
        if(mode==2) {
            Check(std::fabs(d[0][12]-.09f)<2e-5f,"pistol support translation follows gun rather than wandering left controller");
            Check(std::fabs(d[0][13])<2e-5f,"pistol support ignores independent left translation");
        }
        arms::EndPresentationEye();arms::BeginPresentationEye();Check(Draw()==one,"map/support draws stay identical across eyes");arms::EndPresentationEye();
        Check(one!=NativePayload(),"map/support source receives a render correction");
        // Both engagement and release must wait for a newly matched native solve.
        if(mode==0) { mapOpen=mapControls=false;normal=true;arms::g_frame.map=false; }
        else arms::g_frame.twoHand=false;
        arms::PreparePresentation(true);arms::BeginPresentationEye();Check(Draw()==NativePayload(),"map close/support release cannot reuse prior mode correction");arms::EndPresentationEye();
    }
    Reset();normal=false;mapOpen=mapControls=true;originalSource.frame.map=true;adapter::poses[0].source=originalSource;Capture();Target();
    for(int invalid=0;invalid<3;++invalid) {
        phone=invalid==0;loading=invalid==1;vehicle=invalid==2?1:0;
        arms::PreparePresentation(true);Check(!adapter::ready,"phone/loading/vehicle cannot acquire map presentation");
    }
}

static std::array<uint8_t,0x80> playerEntity{},childEntity{},part{},primitive{};
static std::array<uint8_t,16> parentHandle{},childHandle{};
static std::array<uint8_t,0xC4> attachment{};
static std::array<uint8_t,0x1B0> childGfx{};
static std::array<uint8_t,0x28> proxy{};
static std::array<uint8_t,0x60> worldResource{};
static std::array<uint8_t,20> worldTable{};
static std::array<uint8_t*,1> parts{};
static int publishCalls=0;
static constexpr uint64_t PrimitiveHandle=0x88881234ull,WorldHandle=0x77771234ull;
static void __fastcall NativePublish(uint8_t*,void*) { ++publishCalls; }
static uint8_t* __fastcall WriteGet(void* pool,void*,const uint64_t* handle) {
    if(pool==(void*)adapter::VA(0x10EEF898) && *handle==PrimitiveHandle) return primitive.data();
    if(pool==(void*)adapter::VA(0x10EEF7A0) && *handle==WorldHandle) return worldResource.data();
    return nullptr;
}
static Matrix ProviderWorld() {
    Matrix m{};for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
        std::memcpy(&m[r*4+c],provider.data()+0x30+(c*4+r)*4,4);
    return m;
}
static Matrix PrepareAttachment(int side,bool map) {
    Reset();normal=!map;mapOpen=mapControls=map;publishCalls=0;
    originalSource.frame.map=map;originalSource.frame.bonePubRoot=!map;
    originalSource.handWorld[side]=Model(100+(side?.3f:-.3f),200.45f,4.35f);
    adapter::poses[0].source=originalSource;
    playerEntity.fill(0);childEntity.fill(0);parentHandle.fill(0);childHandle.fill(0);attachment.fill(0);
    childGfx.fill(0);part.fill(0);proxy.fill(0);primitive.fill(0);worldResource.fill(0);worldTable.fill(0);
    Store(gfx,8,parentHandle.data());Store(parentHandle,12,playerEntity.data());Store(childHandle,12,childEntity.data());
    Store(childGfx,8,childHandle.data());Store(childEntity,0x78,attachment.data());Store(attachment,0x18,parentHandle.data());
    Store(attachment,0x1C,originalSource.hand[side]);Store(attachment,0x40,originalSource.handWorld[side]);
    const auto offset=Model(.02f,.03f,.04f),world=Multiply(offset,originalSource.handWorld[side]);
    Store(attachment,0x80,offset);Store(childEntity,0x30,world);Store(childGfx,0x170,world);
    parts[0]=part.data();Store(childGfx,0x30,parts.data());Store(childGfx,0x34,1u);
    Store(part,0x34,proxy.data());Store(part,0x40,Identity());Store(proxy,0x1C,PrimitiveHandle);Store(primitive,0x70,WorldHandle);
    const Vec3 fraction{.125f,-.25f,.5f};auto raw=world;raw[12]-=fraction.x;raw[13]-=fraction.y;raw[14]-=fraction.z;
    Store(worldResource,0x10,raw);Store(worldResource,0x50,fraction);
    Store(worldResource,0,worldTable.data());Store(worldTable,4,provider.data());
    adapter::writeGet=WriteGet;adapter::originalPublish=NativePublish;arms::g_wGfx=childGfx.data();return raw;
}
static void AttachedProviders() {
    for(int mode=0;mode<3;++mode) {
        const bool map=mode!=0;const int side=mode==1?0:1;
        const auto raw=PrepareAttachment(side,map);const auto nativeResource=worldResource;const auto nativeAttachment=attachment;
        const auto nativeEntity=childEntity;const auto nativeGfx=childGfx;
        adapter::Publish(childGfx.data(),nullptr);
        Check(publishCalls==1 && adapter::worldMatched==1,"production publication matches local hand child and exact primitive world");
        Check(adapter::worlds[0].side==side,"attachment correction records the owning hand");Target();
        arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);const auto first=ProviderWorld();arms::EndPresentationEye();
        auto expected=raw;expected[12]+=side?.09f:.01f;if(!side) expected[13]-=.06f;
        Check(adapter::Near(first,expected,.00004f),"map/GPS/weapon provider follows correct hand with native fraction retained");
        arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);Check(adapter::Near(ProviderWorld(),first),"world providers share exact two-eye correction");arms::EndPresentationEye();
        Check(worldResource==nativeResource && attachment==nativeAttachment && childEntity==nativeEntity && childGfx==nativeGfx,
            "presentation never changes native world resource, attachment, entity or graphics transform");
        adapter::PresentWorld(worldResource.data(),&WorldHandle,0);Check(adapter::Near(ProviderWorld(),raw),"non-eye world read restores native provider");
        Check(lockDepth==0,"world publication and rendering release locks");
    }
    for(int invalid=0;invalid<6;++invalid) {
        PrepareAttachment(0,true);
        if(invalid==0) Store(attachment,0x18,childHandle.data()); // another entity's child
        if(invalid==1) Store(attachment,0x1C,19); // non-hand bone
        if(invalid==2) { auto wrong=originalSource.handWorld[0];wrong[12]+=.02f;Store(attachment,0x40,wrong); }
        if(invalid==3) Store(childGfx,0x170,Identity());
        if(invalid==4) Store(primitive,0x70,OtherHandle);
        if(invalid==5) { mapOpen=mapControls=false;normal=true; }
        adapter::Publish(childGfx.data(),nullptr);
        Check(publishCalls==1 && adapter::worldMatched==0,"foreign/mismatched attachment cannot acquire correction");
    }
    auto raw=PrepareAttachment(1,true);adapter::Publish(childGfx.data(),nullptr);Target();
    raw[12]+=.02f;Store(worldResource,0x10,raw);
    arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);arms::EndPresentationEye();
    Check(adapter::Near(ProviderWorld(),raw),"changed native world bytes restore uncorrected current data");
    Check(adapter::worldDraws==0,"changed world resource cannot reuse old ownership");
}
