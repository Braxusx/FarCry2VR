static void PhoneAndGadgetPresentation() {
    for(int explicitPhone=0;explicitPhone<2;++explicitPhone) {
        Reset();phone=explicitPhone!=0;
        originalSource.frame.phone=phone;originalSource.frame.weapon=nullptr;
        originalSource.frame.bonePubRoot=!phone;
        arms::g_curWeapon=nullptr;adapter::poses[0].source=originalSource;
        Capture();Target();arms::BeginPresentationEye();const auto first=Draw();
        Check(first!=NativePayload(),"phone hands receive late presentation");
        arms::EndPresentationEye();arms::BeginPresentationEye();
        Check(Draw()==first,"phone skin correction is identical in both eyes");arms::EndPresentationEye();
        arms::g_frame.phone=!phone;arms::PreparePresentation(true);arms::BeginPresentationEye();
        Check(Draw()==NativePayload(),"phone transition rejects a prior source pose");arms::EndPresentationEye();

        const auto raw=PrepareAttachment(1,false);
        phone=explicitPhone!=0;originalSource.frame.phone=phone;originalSource.frame.weapon=nullptr;
        arms::g_curWeapon=nullptr;arms::g_wGfx=nullptr;adapter::poses[0].source=originalSource;
        adapter::Publish(childGfx.data(),nullptr);
        Check(adapter::worldMatched==1,"unarmed local hand gadget matches even without PhoneOut detection");
        Check(depthCalls==0 && arms::g_gadgetDepthNext==1,"publication queues depth but does not mutate native pass state");
        Target();arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);
        const auto corrected=ProviderWorld();arms::EndPresentationEye();
        auto expected=raw;expected[12]+=.09f;
        Check(adapter::Near(corrected,expected,.00004f),"phone model follows the same hand delta");
        Check(adapter::gadgetDraws==1,"gadget runtime counter distinguishes new object correction");
        arms::BeginPresentationEye();adapter::PresentWorld(worldResource.data(),&WorldHandle,0);
        Check(adapter::Near(ProviderWorld(),corrected),"phone attachment correction is identical in both eyes");arms::EndPresentationEye();
        Store(attachment,0x1C,19);adapter::worldMatched=0;
        adapter::Publish(childGfx.data(),nullptr);Check(adapter::worldMatched==0,"unarmed foreign/non-hand items remain native");
    }
    Reset();phone=true;originalSource.frame.phone=true;originalSource.frame.weapon=nullptr;
    arms::g_curWeapon=nullptr;adapter::poses[0].source=originalSource;Capture();Target();
    for(int invalid=0;invalid<4;++invalid) {
        normal=invalid!=0;loading=invalid==1;vehicle=invalid==2?1:0;arms::g_frame.twoHand=invalid==3;
        arms::PreparePresentation(true);Check(!adapter::ready,"phone cannot bypass cinematic/loading/vehicle/support exclusions");
    }
}
static void GadgetDepthOwnership() {
    for(int side=0;side<2;++side) {
        PrepareAttachment(side,true);adapter::Publish(childGfx.data(),nullptr);
        arms::UpdateGadgetDepth();Check(depthCalls==1,"both map and GPS use the existing world-depth setter on game update");
        Store(attachment,0x1C,19);arms::UpdateGadgetDepth();Check(depthCalls==1,"reparented gadget cannot retain depth ownership");
        Store(attachment,0x1C,originalSource.hand[side]);Store(childHandle,12,playerEntity.data());
        arms::UpdateGadgetDepth();Check(depthCalls==1,"reused graphics identity cannot retain depth ownership");
    }
    PrepareAttachment(1,true);adapter::Publish(childGfx.data(),nullptr);
    for(int i=0;i<100;++i) adapter::Publish(childGfx.data(),nullptr);
    Check(arms::g_gadgetDepthNext==1,"repeated publication does not evict gadget ownership");
    Check(lockDepth==0,"deferred depth queue releases its lock");
}
