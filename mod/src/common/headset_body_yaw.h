#pragma once
#include <cmath>
#include <cstdint>

// Physical head yaw is transferred to the native pawn, then removed from the
// render basis. Both the head and controllers continue using the same basis.
namespace fc2vr::body_yaw {
constexpr float Pi = 3.14159265358979323846f;
inline float Wrap(float a) {
    if (!std::isfinite(a)) return 0;
    a = std::remainder(a, 2 * Pi);
    return a;
}
inline bool HeadYaw(float x, float z, float& yaw) {
    const float n = x*x + z*z;
    if (!std::isfinite(n) || n < 0.04f) return false;
    yaw = std::atan2(-x, -z); // XR +Y yaw, native world +Z yaw
    return std::isfinite(yaw);
}
struct Sample {
    std::uintptr_t owner = 0;
    std::uint64_t reference = 0, sequence = 0, receivedMs = 0;
    float headYaw = 0;
    bool tracked = false, onFoot = false;
};
struct State {
    std::uintptr_t owner = 0;
    std::uint64_t reference = 1, consumed = 0;
    float transferred = 0, cameraBodyOffset = 0;
    bool calibrated = false, refused = false;
    void Reset() {
        ++reference; consumed = 0; transferred = 0;
        calibrated = false; refused = false;
    }
    void Owner(std::uintptr_t value) {
        if (owner != value) { owner = value; Reset(); }
    }
    bool Render(float camera, float native, bool nativeValid, float& corrected) {
        if (!std::isfinite(camera)) return false;
        if (nativeValid && std::isfinite(native)) {
            if (!calibrated) { cameraBodyOffset = Wrap(camera-native); calibrated = true; }
            // Use the same native heading that receives the body transfer.
            // The animated game camera can still be one simulation step old.
            corrected = Wrap(native + cameraBodyOffset - transferred);
        } else corrected = Wrap(camera - transferred); // menus/vehicles retain compensation
        return true;
    }
    float Plan(const Sample& s, std::uint64_t now) {
        if (!owner || s.owner != owner || s.reference != reference || !calibrated || refused ||
            !s.sequence || s.sequence <= consumed || !s.tracked || !s.onFoot ||
            !std::isfinite(s.headYaw) || now < s.receivedMs || now-s.receivedMs > 100) return 0;
        consumed = s.sequence;
        const float error = Wrap(s.headYaw-transferred);
        if (std::fabs(error) < 0.001f) return 0;
        const float limit = Pi / 4;
        return error < -limit ? -limit : error > limit ? limit : error;
    }
    void Commit(float observedDelta) { transferred = Wrap(transferred + observedDelta); }
};
} // namespace fc2vr::body_yaw
