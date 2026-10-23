#include "common/pose_frame.h"
#include "fixtures/steady_camera_v230.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace fc2vr::tracking;
namespace {
int checks = 0, failures = 0;
constexpr float pi = 3.14159265358979323846f;
void Check(bool pass, const char* what) {
    ++checks;
    if (!pass) { ++failures; std::fprintf(stderr,"FAIL: %s\n",what); }
}
void Near(float got, float want, const char* what, float tolerance = 2e-5f) {
    Check(std::isfinite(got) && std::fabs(got-want) < tolerance,what);
}
void Near(Vec3 got, Vec3 want, const char* what, float tolerance = 2e-5f) {
    Near(got.x,want.x,what,tolerance); Near(got.y,want.y,what,tolerance); Near(got.z,want.z,what,tolerance);
}
void Near(const GameBasis& got, const GameBasis& want, const char* what) {
    Near(got.P,want.P,what); Near(got.R,want.R,what); Near(got.U,want.U,what); Near(got.F,want.F,what);
}
float Distance(Vec3 a,Vec3 b) { const Vec3 d = Subtract(a,b); return std::sqrt(DotProduct(d,d)); }
GameBasis Camera(Vec3 position,float yaw = pi/2) {
    const Vec3 forward{std::cos(yaw),std::sin(yaw),0};
    return {{forward.y,-forward.x,0},{0,0,1},forward,position};
}
ReferenceFrame Reference(float yaw = 0,float height = 0.2f) {
    return {{2,1.6f,-3},{std::cos(yaw),0,-std::sin(yaw)},
            {-std::sin(yaw),0,-std::cos(yaw)},height};
}
void Bob() {
    PoseFrameBuilder builder;
    PoseFrame frame;
    SteadyInput input{1000,true,false,{10,20,0}};
    const Vec3 eyeLocal{2,1.6f,-3}, handLocal{2.3f,1.3f,-3.5f};
    for (unsigned i = 0; i <= 200; ++i) {
        input.nowMs = 1000 + 16.0*i;
        const float bob = i == 200 ? 0.1f : 0;
        const auto raw = Camera({10,20,1.7f+bob});
        frame = builder.Build(i,raw,Reference(),1,input);
        // Rendering and the game-thread arm adapter use the same production helper.
        const Vec3 eye = frame.WorldPosition(eyeLocal);
        const Vec3 hand = frame.game.Position(frame.ReferencePosition(handLocal));
        const Vec3 relative = frame.game.ReferenceDirection(Subtract(hand,eye));
        Near(relative,{0.3f,-0.3f,-0.5f},"fixed controller-to-eye offset survives camera bob");
        Near(hand,frame.WorldPosition(handLocal),"arm publication agrees with eye transform");
    }
    const Vec3 legacyHand = frame.raw.Position(frame.ReferencePosition(handLocal));
    const float oldMismatch = Distance(legacyHand,frame.WorldPosition(handLocal));
    Near(oldMismatch,0.0948063f,"negative control reproduces old raw/steady origin split");
    Check(oldMismatch > 0.09f,"fixture actually exercises smoothing");
    Near(frame.raw.P.z,1.8f,"raw native origin retained separately");
    std::printf("legacy bob disagreement %.7f game units; shared disagreement %.7f\n",
        oldMismatch,Distance(frame.WorldPosition(handLocal),frame.game.Position(frame.ReferencePosition(handLocal))));
}
void Yaw() {
    PoseFrameBuilder builder;
    PoseFrame frame;
    SteadyInput input{1000,true,false,{0,0,0}};
    const Vec3 localDirection{0,0,-1};
    for (unsigned i = 0; i <= 200; ++i) {
        input.nowMs = 1000+16.0*i; input.playerPosition = {0,0.08f*i,0};
        const float yaw = pi/2+(i == 200 ? 0.05f : 0);
        const auto raw = Camera(Add(input.playerPosition,{0,0,1.7f}),yaw);
        frame = builder.Build(i,raw,Reference(),1,input);
        Near(frame.WorldDirection(localDirection),frame.game.Direction(frame.ReferenceDirection(localDirection)),
             "hands and eye direction share smoothed yaw");
        Near(DotProduct(frame.game.R,frame.game.F),0,"effective basis stays orthogonal");
    }
    Check(Distance(frame.raw.F,frame.game.F) > 0.04f,"negative control exercises native/steady yaw split");
    input.turning = true; input.nowMs += 16;
    frame = builder.Build(201,Camera(Add(input.playerPosition,{0,0,1.7f}),pi/2+0.1f),Reference(),1,input);
    Near(frame.game.F,frame.raw.F,"stick turn follows raw heading immediately");
    input.turning = false; input.nowMs += 16;
    frame = builder.Build(202,Camera(Add(input.playerPosition,{0,0,1.7f}),pi/2+0.5f),Reference(),1,input);
    Near(frame.game.F,frame.raw.F,"large snap turn follows immediately");
}
void Policy() {
    PoseFrameBuilder builder;
    SteadyInput input{1000,true,false,{0,0,0}};
    for (unsigned i = 0; i < 1000; ++i) {
        input.nowMs += 16;
        if (i == 800) input.nowMs += 500;   // gap: reset
        if (i == 850) input.nowMs -= 16;    // zero dt: reset
        if (i == 900) input.nowMs -= 32;    // backwards clock: reset
        input.eligible = !(i >= 700 && i < 720);
        input.turning = i >= 600 && i < 650;
        const float t = static_cast<float>(i)*0.016f;
        const float travel = i < 250 ? 5*t : i < 300 ? 20.0f+0.5f*(t-4) : 5*t;
        input.playerPosition = {i >= 450 ? 10.0f : 0.0f,travel,0};
        const float height = 1.7f+(i >= 350 && i < 400 ? -0.6f : 0.0f);
        const Vec3 off{0.03f*std::cos(t*12),0.01f,height+0.1f*std::sin(t*12)+(i == 500 ? 1.0f : 0.0f)};
        const auto raw = Camera(Add(input.playerPosition,off),3.13f+0.06f*std::sin(t*12));
        const auto expected = legacy_v230::Update(raw,input);
        const auto frame = builder.Build(i,raw,Reference(),1,input);
        Near(frame.game,expected,"extracted SteadyCamera policy unchanged");
        Near(frame.raw,raw,"raw camera copy never modified");
    }
}
void Once() {
    PoseFrameBuilder builder,control;
    SteadyInput input{1000,true,false,{0,0,0}};
    const auto raw = Camera({0,0,1.7f});
    builder.Build(0,raw,Reference(),1,input); control.Build(0,raw,Reference(),1,input);
    input.nowMs += 16;
    const auto first = builder.Build(1,Camera({0.1f,0,1.8f}),Reference(),2,input);
    control.Build(1,Camera({0.1f,0,1.8f}),Reference(),2,input);
    // Repeated consumer/eye calls cannot advance smoothing or replace the sample.
    for (int i = 0; i < 4; ++i) {
        const auto repeat = builder.Build(1,Camera({100,200,300},0),Reference(1,-2),7,{9000,false,true,{1,2,3}});
        Near(repeat.game,first.game,"duplicate sample cannot change effective frame");
        Near(repeat.raw,first.raw,"duplicate sample cannot replace raw camera");
        Near(repeat.reference.origin,first.reference.origin,"recenter snapshot immutable");
        Near(repeat.reference.heightOffset,first.reference.heightOffset,"height snapshot immutable");
        Near(repeat.worldScale,2,"scale snapshot immutable");
        Check(repeat.sequence == 1,"shared sequence retained");
    }
    input.nowMs += 16;
    const auto next = builder.Build(2,raw,Reference(),1,input);
    Near(next.game,control.Build(2,raw,Reference(),1,input).game,"duplicates did not advance smoothing history");
    input.eligible = false; input.nowMs += 16;
    const auto bypass = builder.Build(3,Camera({1,2,3},0.4f),Reference(),1,input);
    Near(bypass.game,bypass.raw,"SteadyCamera disabled/special-control bypass preserves native frame");
    input.eligible = true; input.nowMs += 16;
    const auto resumed = builder.Build(4,Camera({1,2,3},0.4f),Reference(),1,input);
    Near(resumed.game,resumed.raw,"mode reentry resets without dragging old bob history");
}
void Inverse() {
    PoseFrameBuilder builder;
    unsigned sequence = 0;
    for (float scale : {0.5f,1.0f,2.0f}) for (float height : {-0.2f,0.0f,0.2f}) {
        for (int angle = -180; angle <= 180; angle += 15) {
            const float yaw = angle*pi/180;
            const auto f = builder.Build(sequence++,Camera({4,5,1.7f},yaw),Reference(yaw/2,height),scale,{});
            const Vec3 p{2.3f,1.2f,-3.7f}, direction{0.3f,0.4f,-0.5f};
            Near(f.LocalPosition(f.ReferencePosition(p)),p,"scaled XR/ref position round trip");
            Near(f.LocalDirection(f.ReferenceDirection(direction)),direction,"XR/ref direction round trip");
            Near(f.game.ReferencePosition(f.WorldPosition(p)),f.ReferencePosition(p),"world/ref position round trip");
            Near(f.game.ReferenceDirection(f.WorldDirection(direction)),f.ReferenceDirection(direction),"world/ref direction round trip");
            Near(f.LocalPosition(f.game.ReferencePosition(f.WorldPosition(p))),p,"reticle/arc world-to-XR round trip");
            const Vec3 left{-std::sin(0.03f),0,-std::cos(0.03f)}, right{std::sin(0.02f),0,-std::cos(0.02f)};
            Near(DotProduct(f.WorldDirection(left),f.WorldDirection(right)),std::cos(0.05f),"canted eye orientation difference preserved");
            const Vec3 up = f.WorldDirection({0,1,0});
            Near(DotProduct(up,up),1,"height/scale never stretch up direction");
        }
    }
}
void Bone() {
    PoseFrameBuilder builder;
    const auto f = builder.Build(5,Camera({10,20,1.7f}),Reference(),1,{});
    const Vec3 refHand = f.ReferencePosition({2.3f,1.3f,-3.5f});
    const GameBasis published = f.game;   // arms::Frame copy, not a Camera-bone offset
    const Vec3 target = published.Position(refHand), oldBone{10,20,1.6f};
    for (Vec3 boneDelta : {Vec3{0,0,0},Vec3{0.1f,0,0.2f},Vec3{-0.2f,0.1f,-0.1f}}) {
        const Vec3 animatedBone = Add(oldBone,boneDelta);
        Near(published.Position(refHand),target,"later Camera-bone motion cannot move tracking origin");
        const Vec3 legacyOrigin = Add(animatedBone,Subtract(f.game.P,oldBone));
        if (DotProduct(boneDelta,boneDelta) > 0) {
            Check(Distance(Add(legacyOrigin,published.Direction(refHand)),target) > 0.2f,
                  "negative control: old bone-relative origin moves a stationary hand");
        }
    }
}
} // namespace
int main(int argc,char** argv) {
    if (argc != 2) return 2;
    const char* group = argv[1];
    if (!std::strcmp(group,"bob")) Bob();
    else if (!std::strcmp(group,"yaw")) Yaw();
    else if (!std::strcmp(group,"policy")) Policy();
    else if (!std::strcmp(group,"once")) Once();
    else if (!std::strcmp(group,"inverse")) Inverse();
    else if (!std::strcmp(group,"bone")) Bone();
    else return 2;
    std::printf("%s: %d checks, %d failures\n",group,checks,failures);
    return failures ? 1 : 0;
}
