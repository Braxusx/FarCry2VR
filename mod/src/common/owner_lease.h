#pragma once
#include <cstdint>

namespace fc2vr::owner {
// Observed identities only. This does not replace native lifetime/refcount or
// worker ownership proof, nor detect reuse that leaves every observed field equal.
struct Key {
    std::uintptr_t actor=0,data=0,graphics=0,rig=0,palette=0;
    int count=0,left=-1,right=-1;
    std::uintptr_t weapon=0,handle=0,entity=0,modelGraphics=0,model=0,modelPalette=0,attachment=0;
    int modelCount=0;
    bool Valid() const {
        if(!actor || !data || !graphics || !rig || !palette || count<1 || count>512
            || left<0 || right<0 || left>=count || right>=count || left==right) return false;
        if(!weapon) return !handle && !entity && !modelGraphics && !model && !modelPalette && !attachment && !modelCount;
        return handle && entity && modelGraphics && model && modelPalette && attachment && modelCount>0 && modelCount<=512;
    }
    bool operator==(const Key& k) const {
        return actor==k.actor && data==k.data && graphics==k.graphics && rig==k.rig && palette==k.palette
            && count==k.count && left==k.left && right==k.right && weapon==k.weapon && handle==k.handle
            && entity==k.entity && modelGraphics==k.modelGraphics && model==k.model && modelPalette==k.modelPalette
            && attachment==k.attachment && modelCount==k.modelCount;
    }
};
constexpr std::uint64_t MaxAgeMs=250;
struct Snapshot { Key key{}; std::uint64_t epoch=0,observedMs=0; bool valid=false; };
inline bool Fresh(const Snapshot& s,std::uint64_t now) {
    return s.valid && s.epoch && now>=s.observedMs && now-s.observedMs<=MaxAgeMs;
}
inline bool Matches(const Snapshot& a,const Snapshot& b) {
    return a.valid && b.valid && a.epoch && a.epoch==b.epoch && a.key==b.key;
}
class Tracker {
    Snapshot state{};
public:
    bool Observe(const Key& key,std::uint64_t now) {
        const bool valid=key.Valid();
        if(state.valid==valid && (!valid || (state.key==key && Fresh(state,now)))) { state.observedMs=now; return false; }
        ++state.epoch; if(!state.epoch) ++state.epoch;
        state.key=valid?key:Key{}; state.valid=valid; state.observedMs=now; return true;
    }
    Snapshot Read() const { return state; }
};
// Frozen presentation may replace only a complete, bounded palette update.
inline bool CoversPalette(int count,const int* map,int bones) {
    if(!map || count<1 || count>512 || bones<1 || bones>64) return false;
    bool seen[64]{};
    for(int i=0;i<count;++i) {
        if(map[i]<-1 || map[i]>=bones) return false;
        if(map[i]>=0) seen[map[i]]=true;
    }
    for(int i=0;i<bones;++i) if(!seen[i]) return false;
    return true;
}
} // namespace fc2vr::owner
