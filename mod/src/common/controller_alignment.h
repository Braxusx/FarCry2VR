// Runtime-selected calibration. Never writes back into the user's base settings.
#pragma once
#include <string_view>

namespace fc2vr::controllers {
inline bool EqualName(std::string_view a, std::string_view b) {
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i) {
        const auto lower=[](char c) { return c>='A' && c<='Z' ? char(c-'A'+'a') : c; };
        if (lower(a[i])!=lower(b[i])) return false;
    }
    return true;
}
inline bool QuestSystem(std::string_view name) {
    // systemName is runtime-provided text, not a standardized hardware ID.
    // Recognize explicit Quest model names; generic Oculus/Meta/SteamVR names
    // and future/unknown models keep the base calibration.
    constexpr std::string_view names[]={
        "Oculus Quest", "Oculus Quest 2", "Oculus Quest2", "Oculus Quest Pro",
        "Meta Quest", "Meta Quest 2", "Meta Quest Pro", "Meta Quest 3", "Meta Quest 3S",
        "Quest", "Quest 2", "Quest Pro", "Quest 3", "Quest 3S"
    };
    for (auto candidate:names) if (EqualName(name,candidate)) return true;
    return false;
}
class Detection {
    bool quest_=false, touch_[2]{};
public:
    void ClearProfiles() { touch_[0]=touch_[1]=false; }
    void System(std::string_view name, bool querySucceeded) {
        quest_=querySucceeded && QuestSystem(name); ClearProfiles();
    }
    bool QuestHeadset() const { return quest_; }
    void Profile(int side, std::string_view path, bool querySucceeded) {
        if (side<0 || side>1) return;
        // This is the Touch profile the mod supplies bindings for. OpenXR may
        // emulate it, so it is insufficient without the headset check above.
        touch_[side]=querySucceeded && path=="/interaction_profiles/oculus/touch_controller";
    }
    bool QuestTouch(int side) const { return side>=0 && side<2 && quest_ && touch_[side]; }
};
struct Alignment {
    bool freePalm=false, weaponPalm=false;
    float macheteRoll=0;
};
struct QuestPreset {
    bool automatic=true, freePalm=true, weaponPalm=true;
    float macheteRollOffset=30;
};
inline Alignment Resolve(const Alignment& base, const QuestPreset& quest, bool questTouch) {
    if (!quest.automatic || !questTouch) return base;
    return {quest.freePalm,quest.weaponPalm,base.macheteRoll+quest.macheteRollOffset};
}
} // namespace fc2vr::controllers
