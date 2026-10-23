#pragma once
#include <cmath>
#include <cstdint>

namespace fc2vr::authority {
constexpr std::uint64_t MaxAgeMs = 100;
struct Stamp {
    std::uint64_t sequence = 0, receivedMs = 0, generation = 0;
    std::uint64_t handGeneration[2]{};
    std::int64_t predictedTime = 0;   // XR display clock; never used for elapsed freshness
    bool focused = false, synced = false;
};
inline bool SameContext(const Stamp& source, const Stamp& current, int hand = -1) {
    return source.sequence && source.sequence <= current.sequence && source.receivedMs <= current.receivedMs
        && source.focused && source.synced && current.focused && current.synced
        && source.generation == current.generation
        && (hand < 0 || (hand < 2 && source.handGeneration[hand] == current.handGeneration[hand]));
}
inline bool Fresh(const Stamp& source, const Stamp& current, std::uint64_t nowMs, int hand = -1) {
    return SameContext(source,current,hand) && nowMs >= source.receivedMs
        && nowMs-source.receivedMs <= MaxAgeMs && nowMs >= current.receivedMs
        && nowMs-current.receivedMs <= MaxAgeMs;
}
struct PoseEvidence {
    bool active = false, positionValid = false, orientationValid = false;
    bool positionTracked = false, orientationTracked = false;
    bool Valid() const { return active && positionValid && orientationValid; }
    bool Tracked() const { return Valid() && positionTracked && orientationTracked; }
};
template<class Pose> bool FinitePosition(const Pose& p) {
    return std::isfinite(p.position.x) && std::isfinite(p.position.y) && std::isfinite(p.position.z);
}
template<class Pose> bool FiniteOrientation(const Pose& p) {
    const auto& q = p.orientation; const float n = q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return std::isfinite(n) && n > 0.25f && n < 4;
}
// A held visual pose never changes the original location evidence.
template<class Pose> struct PoseResult { Pose pose{}; bool visualValid = false, held = false; };
template<class Pose> class PoseCache {
    Pose saved{};
    bool have = false;
public:
    PoseResult<Pose> Resolve(const Pose& raw, const PoseEvidence& evidence, bool hold) {
        if (evidence.Valid()) { saved = raw; have = true; return {raw,true,false}; }
        if (!hold || !have) return {};
        if (evidence.active && evidence.orientationValid) saved.orientation = raw.orientation;
        return {saved,true,true};
    }
    void Clear() { have = false; saved = {}; }
};
class RearmGate {
    bool armed = false;
public:
    bool Update(bool current, bool neutral) {
        if (!current) armed = false;
        else if (neutral) armed = true;
        return armed;
    }
    void Clear() { armed = false; }
};
class SampleCursor {
    std::uint64_t generation = 0, sequence = 0;
public:
    bool Take(const Stamp& stamp) {
        if (!stamp.sequence || (generation == stamp.generation && stamp.sequence <= sequence)) return false;
        generation = stamp.generation; sequence = stamp.sequence; return true;
    }
    void Clear() { generation = sequence = 0; }
};
inline float FiniteAxis(float v, float low = -1, float high = 1) {
    return !std::isfinite(v) ? 0 : v < low ? low : v > high ? high : v;
}
inline bool SameOwner(const void* leased, const void* current) { return leased && leased == current; }
} // namespace fc2vr::authority
