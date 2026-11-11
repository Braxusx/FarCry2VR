// Publication proves local hand ownership; render-pass mutation is deferred to
// the same game-thread camera update that already handles body/weapon depth.
struct GadgetDepthEntry { uint8_t* graphics=nullptr;uint8_t* entity=nullptr; };
static GadgetDepthEntry g_gadgetDepth[64];static unsigned g_gadgetDepthNext=0;
static uint8_t* GadgetEntity(uint8_t* gfx) {
    if(!gfx || IsBadReadPtr(gfx+8,4)) return nullptr;
    auto handle=*(uint8_t**)(gfx+8);
    return handle && !IsBadReadPtr(handle+12,4) ? *(uint8_t**)(handle+12) : nullptr;
}
static void QueueGadgetDepth(uint8_t* gfx,uint8_t* entity) { // g_cs held
    for(const auto& item:g_gadgetDepth) if(item.graphics==gfx && item.entity==entity) return;
    g_gadgetDepth[g_gadgetDepthNext++%64]={gfx,entity};
}
static bool LocalGadgetAttachment(uint8_t* entity,uint8_t* player) {
    // Direct hand gadgets and exactly one level of their map markers. Recheck
    // the live parent chain so discarded/reused graphics pointers do not qualify.
    for(unsigned level=0;level<2;++level) {
        if(!entity || entity==player || IsBadReadPtr(entity+0x78,4)) return false;
        auto at=*(uint8_t**)(entity+0x78);
        if(!at || IsBadReadPtr(at+0x18,8)) return false;
        auto handle=*(uint8_t**)(at+0x18);
        if(!handle || IsBadReadPtr(handle+12,4)) return false;
        auto parent=*(uint8_t**)(handle+12);
        if(parent==player) {
            const int bone=*(int*)(at+0x1C);
            return bone>=0 && (bone==g_slot[12] || bone==g_slot[20]);
        }
        entity=parent;
    }
    return false;
}
static void UpdateGadgetDepth() {
    if(!g_nearViewport) return;
    auto player=GadgetEntity((uint8_t*)g_playerGfx);if(!player) return;
    GadgetDepthEntry entries[64];Lock();memcpy(entries,g_gadgetDepth,sizeof entries);Unlock();
    for(const auto& item:entries) {
        if(!item.graphics || GadgetEntity(item.graphics)!=item.entity || !LocalGadgetAttachment(item.entity,player)) continue;
        WorldBodyOcclusion(item.graphics,item.graphics+0xE0,"hand gadget/marker");
    }
}
