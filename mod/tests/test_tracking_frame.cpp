#include "common/tracking_frame.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using fc2vr::tracking::ReferenceFrame;
using fc2vr::tracking::Vec3;
static_assert(sizeof(Vec3) == 3 * sizeof(float), "Preserve the production V3 layout");

namespace {
int checks = 0, failures = 0;
constexpr float pi = 3.14159265358979323846f;
float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 Scale(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
void Check(bool pass, const char* message) {
    ++checks;
    if (!pass) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void Near(float got, float want, const char* message, float tolerance = 2e-5f) {
    Check(std::isfinite(got) && std::fabs(got-want) < tolerance, message);
}
void Near(Vec3 got, Vec3 want, const char* message) {
    Near(got.x, want.x, message); Near(got.y, want.y, message); Near(got.z, want.z, message);
}
ReferenceFrame Yaw(float radians, float height = 0) {
    // Same horizontal orthonormal basis produced by DoRecenter.
    return {{2, 1.6f, -3}, {std::cos(radians), 0, -std::sin(radians)},
            {-std::sin(radians), 0, -std::cos(radians)}, height};
}
void Position() {
    ReferenceFrame f{{2, 1.6f, -3}, {1,0,0}, {0,0,-1}, 0.2f};
    Near(f.Position({2.25f, 1.9f, -3.5f}), {0.25f, 0.5f, -0.5f}, "position subtracts origin and adds height once");
    Near(f.Position(f.origin), {0, 0.2f, 0}, "recenter origin retains configured height");
    f.heightOffset = -0.3f;
    Near(f.Position({2.25f, 1.9f, -3.5f}), {0.25f, 0, -0.5f}, "negative height offset");
    f = Yaw(pi/2, 0.2f);
    Near(f.Position({1, 1.6f, -3}), {0, 0.2f, -1}, "position at ninety-degree recenter");
    Near(Scale(f.Position({1, 1.6f, -3}), 2), {0, 0.4f, -2}, "world scale remains outside tracking conversion");
}
void Direction() {
    const Vec3 v{0.3f, 0.4f, -0.5f};
    for (float height : {-0.5f, 0.0f, 0.2f, 1.0f}) {
        ReferenceFrame f{{20, -50, 7}, {1,0,0}, {0,0,-1}, height};
        Near(f.Direction(v), v, "directions ignore origin and height");
        Near(f.Direction({0,0,-1}), {0,0,-1}, "level forward must not tilt");
        Near(f.Direction({0,1,0}), {0,1,0}, "up axis must stay unit length");
    }
    Near(Yaw(pi/2, 0.2f).Direction({-1,0,0}), {0,0,-1}, "direction at ninety-degree recenter");
    // Negative control: the removed ToRef formula pitches a level forward axis.
    const Vec3 legacy{0, 0.2f, -1};
    const float legacyTilt = std::atan2(legacy.y, -legacy.z) * 180/pi;
    Near(legacyTilt, 11.309933f, "old formula reproduces height-induced tilt");
    Check(std::fabs(Dot(legacy, Vec3{0,1.2f,0})) > 0.2f, "old forward/up axes are not orthogonal");
}
void Rotation() {
    for (int angle = -180; angle <= 180; angle += 5) {
        const auto f = Yaw(angle*pi/180, 0.2f);
        const Vec3 forward = f.Direction({0,0,-1}), up = f.Direction({0,1,0});
        Near(Dot(forward,forward), 1, "forward remains normalized");
        Near(Dot(up,up), 1, "up remains normalized");
        Near(Dot(forward,up), 0, "forward/up remain orthogonal");
        const Vec3 a{0.3f,-0.4f,0.8f}, b{-0.2f,0.7f,0.1f};
        Near(Dot(f.Direction(a), f.Direction(b)), Dot(a,b), "rotated directions preserve dot products");
        Near(Dot(f.Direction(a), f.Direction(a)), Dot(a,a), "rotated direction preserves length");
    }
}
void Stereo() {
    // Non-parallel eye axes: do not silently replace both eyes with a shared orientation.
    const Vec3 left{-std::sin(0.03f), 0, -std::cos(0.03f)};
    const Vec3 right{std::sin(0.02f), 0, -std::cos(0.02f)};
    for (float height : {-0.25f, 0.0f, 0.2f}) {
        const auto f = Yaw(0.7f, height);
        const Vec3 l = f.Direction(left), r = f.Direction(right);
        Near(Dot(l,r), std::cos(0.05f), "asymmetric eye angle preserved");
        Near(l.y, 0, "left eye stays level"); Near(r.y, 0, "right eye stays level");
        Check(Dot(Sub(l,r), Sub(l,r)) > 0.002f, "eye orientations remain distinct");
        const Vec3 lp = f.Position({1.968f,1.6f,-3}), rp = f.Position({2.032f,1.6f,-3});
        Near(Dot(Sub(lp,rp), Sub(lp,rp)), 0.064f*0.064f, "IPD preserved");
    }
}
void Affine() {
    const Vec3 p{0.25f, 1.85f, -4.5f};
    for (int angle = -180; angle <= 180; angle += 15) {
        auto f = Yaw(angle*pi/180, 0.2f);
        Near(Sub(f.Position(p), f.Position(f.origin)), f.Direction(Sub(p,f.origin)), "position difference equals transformed displacement");
        const Vec3 adjusted = f.Position(p);
        f.heightOffset = 0;
        Near(f.Position(p), f.Direction(Sub(p,f.origin)), "zero offset preserves original position behavior");
        Near(Sub(adjusted, f.Position(p)), {0,0.2f,0}, "height changes translation only");
        Near(f.Direction({0,0,0}), {0,0,0}, "zero direction remains zero");
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "Specify a test group\n"); return 2; }
    const char* group = argv[1];
    if (!std::strcmp(group,"position")) Position();
    else if (!std::strcmp(group,"direction")) Direction();
    else if (!std::strcmp(group,"rotation")) Rotation();
    else if (!std::strcmp(group,"stereo")) Stereo();
    else if (!std::strcmp(group,"affine")) Affine();
    else { std::fprintf(stderr, "Unknown group: %s\n", group); return 2; }
    std::printf("%s: %d checks, %d failures\n", group, checks, failures);
    return failures ? 1 : 0;
}
