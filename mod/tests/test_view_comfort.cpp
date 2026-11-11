#include "common/view_comfort.h"
#include <cstdio>
#include <limits>

using namespace fc2vr::tracking;
namespace {
constexpr float pi=3.14159265358979323846f;
int checks=0,failures=0;
void Check(bool ok,const char* message) {
    ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",message); }
}
void Near(float a,float b,const char* message,float epsilon=0.00003f) {
    Check(std::isfinite(a)&&std::fabs(a-b)<epsilon,message);
}
void Near(Vec3 a,Vec3 b,const char* message,float epsilon=0.00003f) {
    Near(a.x,b.x,message,epsilon);Near(a.y,b.y,message,epsilon);Near(a.z,b.z,message,epsilon);
}
struct Sample { BodyPose scene;GameBasis raw; };
Sample Make(std::uint64_t sequence,double time,std::uint64_t shot,double shotTime,
            float look,Vec3 offset,float residual,Vec3 root={10,20,3}) {
    Sample s;
    auto& scene=s.scene;scene.valid=true;scene.sequence=sequence;scene.timeMs=time;
    scene.skeleton=1;scene.graphics=2;scene.root[0]=scene.root[5]=scene.root[10]=scene.root[15]=1;
    scene.root[12]=root.x;scene.root[13]=root.y;scene.root[14]=root.z;
    auto& native=scene.nativeView;native.valid=true;native.pawnData=3;native.weapon=4;
    native.timeMs=time-.1;native.shotSequence=shot;native.shotTimeMs=shotTime;
    native.angles[2]=native.angles[8]=look;
    const float yaw=look+pi/2;
    const Vec3 forward{std::cos(yaw),std::sin(yaw),0},right{forward.y,-forward.x,0};
    s.raw.P=Add(Add(Add(root,Scale(right,offset.x)),Scale(forward,offset.y)),Vec3{0,0,offset.z});
    s.raw.F={std::cos(yaw+residual),std::sin(yaw+residual),0};s.raw.R={s.raw.F.y,-s.raw.F.x,0};
    scene.camera=s.raw.P;return s;
}
ViewComfortResult Run(ShotViewComfort& helper,const Sample& s,bool eligible=true) {
    return helper.Update(s.raw,s.scene,eligible);
}
// Small owner-capture numeric fixture: onfoot-14, rows 2406/2408/2516 of
// work/evidence-onfoot-15-before/fc2vr_onfoot_trace.csv, 2026-10-29.
// Local right/forward/up offsets and yaw residual from matched LookAngles.
// Positions and receipt times below are rebased; no game assets are embedded.
constexpr float look=-2.88913274f,restYaw=-0.00020196201f;
const Vec3 rest{0.02684672111f,0.12166264400f,1.671349f};
const Vec3 firstShot{0.02672482523f,0.12119011316f,1.671722f};
const Vec3 burstShot{0.02729714163f,0.11852053287f,1.673206f};

void CapturedAndTurning() {
    ShotViewComfort helper;
    const auto baseline=Make(1,1000,0,0,look,rest,restYaw);
    const auto initial=Run(helper,baseline);Near(initial.basis.P,baseline.raw.P,"idle position unchanged");
    Check(!initial.active,"no suppression before a confirmed shot");
    const auto shot=Make(2,1022,1,1021.3,look,firstShot,0.00045581073f);
    const auto result=Run(helper,shot);
    Check(result.active&&result.positionSuppressed&&result.yawSuppressed,"confirmed shot activates both corrections");
    Near(result.basis.P,baseline.raw.P,"captured shot translation removed exactly");
    Near(result.basis.F,baseline.raw.F,"captured shot yaw removed exactly");
    Near(shot.scene.camera,shot.raw.P,"native camera ownership unchanged");
    Near(shot.scene.nativeView.angles[2],look,"native look authority unchanged");

    // Captured deliberate turns matched same-sample native look +pi/2 within
    // 0.01163 degrees (240 rows); adjacent samples were ~2.65 degrees apart.
    // Compose that deliberate turn with captured burst recoil and locomotion.
    const auto turn=Make(3,1033,2,1032,look+0.45f,burstShot,-0.00382089374f,{11,21,3});
    const auto turned=Run(helper,turn);
    const auto expected=Make(3,1033,2,1032,look+0.45f,rest,restYaw,{11,21,3});
    Near(turned.basis.F,expected.raw.F,"deliberate turn during burst preserved exactly");
    Near(turned.basis.P,expected.raw.P,"root locomotion and rotating baseline preserved");
    const auto repeat=Run(helper,turn);
    Near(repeat.basis.P,turned.basis.P,"repeated scene stays identical");
    Near(repeat.basis.F,turned.basis.F,"repeated scene does not integrate yaw again");
    Check(repeat.active,"repeated scene cannot advance recovery");
}

void RecoveryAndContinuousFire() {
    ShotViewComfort helper;
    Run(helper,Make(1,1000,0,0,look,rest,restYaw));
    Run(helper,Make(2,1011,1,1010,look,firstShot,0.03f));
    for(unsigned i=3;i<70;++i) {
        const double time=1011+100*(i-2);
        const auto sample=Make(i,time,i-1,time-1,look,burstShot,0.03f);
        Check(Run(helper,sample).active,"continuous shots refresh timeout beyond five seconds");
    }
    const double lastShotTime=7710;
    for(unsigned i=0;i<5;++i) {
        const auto sample=Make(70+i,7811+100*i,68,lastShotTime,look,rest,restYaw);
        Check(Run(helper,sample).active,"recovery needs minimum age and settling interval");
    }
    const auto settled=Make(75,8311,68,lastShotTime,look,rest,restYaw);
    const auto done=Run(helper,settled);
    Check(!done.active,"recovery releases after measured neutral return");
    Near(done.basis.P,settled.raw.P,"release has no translation jump at neutral");
    Near(done.basis.F,settled.raw.F,"release has no yaw jump at neutral");

    ShotViewComfort timeout;
    Run(timeout,Make(1,1000,0,0,look,rest,restYaw));
    Run(timeout,Make(2,1011,1,1010,look,firstShot,0.03f));
    ViewComfortResult last;
    for(unsigned i=3;i<=54;++i) last=Run(timeout,Make(i,1011+100*(i-2),1,1010,look,burstShot,0.03f));
    Check(!last.active,"stuck animation fails open five seconds after last shot");
}

void Boundaries() {
    auto start=[](ShotViewComfort& helper) {
        Run(helper,Make(1,1000,0,0,look,rest,restYaw));
        Run(helper,Make(2,1011,1,1010,look,firstShot,0.03f));
    };
    ShotViewComfort stance;start(stance);
    auto crouch=Make(3,1022,1,1010,look,{rest.x,rest.y,1.1f},.03f);
    crouch.scene.nativeView.desiredStance=crouch.scene.nativeView.effectiveStance=1;
    const auto crouched=Run(stance,crouch);
    Check(!crouched.active,"stance change clears shot baseline");
    Near(crouched.basis.P,crouch.raw.P,"crouch follows native height immediately");
    ShotViewComfort transition;start(transition);
    auto blend=crouch;blend.scene.nativeView.effectiveStance=0;
    Check(!Run(transition,blend).active,"stance transition bypasses suppression");

    for(int field=0;field<4;++field) {
        ShotViewComfort owner;start(owner);auto changed=Make(3,1022,1,1010,look,burstShot,.03f);
        if(field==0) ++changed.scene.skeleton;
        if(field==1) ++changed.scene.graphics;
        if(field==2) ++changed.scene.nativeView.pawnData;
        if(field==3) ++changed.scene.nativeView.weapon;
        const auto result=Run(owner,changed);
        Check(!result.active,"owner or weapon change resets baseline");Near(result.basis.P,changed.raw.P,"new owner receives native camera");
    }
    for(int invalid=0;invalid<8;++invalid) {
        ShotViewComfort helper;start(helper);auto sample=Make(3,1022,1,1010,look,burstShot,.03f);
        if(invalid==0) sample.scene.valid=false;
        if(invalid==1) sample.scene.nativeView.valid=false;
        if(invalid==2) sample.scene.nativeView.angles[2]=std::numeric_limits<float>::quiet_NaN();
        if(invalid==3) sample.scene.nativeView.timeMs-=10;
        if(invalid==4) sample.scene.nativeView.angles[8]+=.1f;
        if(invalid==5) sample.scene.camera.x+=.02f;
        if(invalid==6) sample.scene.nativeView.shotTimeMs+=100;
        const auto result=Run(helper,sample,invalid!=7);
        Check(!result.active,"invalid/control sample bypasses and resets");
        Near(result.basis.P,sample.raw.P,"invalid sample preserves raw position");
        const auto next=Run(helper,Make(4,1033,1,1010,look,burstShot,.03f));
        Check(!next.active,"reset cannot learn a baseline from an ongoing shot");
    }
    ShotViewComfort posture;start(posture);
    const auto big=Make(3,1022,1,1010,look,{rest.x,rest.y,rest.z-.2f},.03f);
    Check(!Run(posture,big).active,"large unlabelled posture change fails open");
    ShotViewComfort fresh;
    Check(!Run(fresh,Make(1,1000,1,999,look,burstShot,.03f)).active,"first observed shot without pre-shot baseline passes through");
}

// Geometry contract for consumers of the separated bases. This verifies the
// coordinate conversion and independent smoothing; it does not exercise the
// native hooks or prove that every consumer selects the gameplay basis.
void GameplayGeometry() {
    auto unit=[](Vec3 v) { return Scale(v,1/std::sqrt(DotProduct(v,v))); };
    for(float scale:{.7f,1.0f,1.4f}) {
        ShotViewComfort comfort;
        PoseFrameBuilder visualBuilder,gameplayBuilder;
        ReferenceFrame reference;
        reference.origin={.1f,.05f,-.3f};reference.heightOffset=.13f;
        reference.right={std::cos(.23f),0,-std::sin(.23f)};
        reference.forward={-std::sin(.23f),0,-std::cos(.23f)};
        for(unsigned step=0;step<3;++step) {
            const auto s=Make(step+1,1000+11*step,step,step?999+11*step:0,
                look+(step==2?.45f:0),step?burstShot:rest,step?.04f:restYaw,
                {10+.1f*step,20+.2f*step,3});
            const auto result=Run(comfort,s);
            SteadyInput input;input.eligible=true;input.turning=step==2;
            input.nowMs=s.scene.timeMs;
            input.playerPosition={s.scene.root[12],s.scene.root[13],s.scene.root[14]};
            const auto visual=visualBuilder.Build(step+1,result.basis,reference,scale,input);
            const auto gameplay=gameplayBuilder.Build(step+1,s.raw,reference,scale,input);
            const Vec3 trackedOrigin{.28f,1.32f,-.44f},trackedAim=unit({.23f,.08f,-.97f});
            const Vec3 refOrigin=visual.ReferencePosition(trackedOrigin);
            const Vec3 refAim=visual.ReferenceDirection(trackedAim);
            const Vec3 visibleOrigin=visual.game.Position(refOrigin);
            const Vec3 visibleAim=visual.game.Direction(refAim);
            const Vec3 actualOrigin=gameplay.game.Position(visual.game.ReferencePosition(visibleOrigin));
            const Vec3 actualAim=unit(gameplay.game.Direction(visual.game.ReferenceDirection(visibleAim)));
            Near(actualOrigin,gameplay.game.Position(Scale(reference.Position(trackedOrigin),scale)),
                "gameplay shot origin retains native camera policy with scale applied once");
            Near(actualAim,gameplay.game.Direction(reference.Direction(trackedAim)),
                "gameplay shot direction retains native recoil during a deliberate turn");

            // Hook_PlayerShot retains the native direction-minus-camera term.
            // Its camera source and deviation must not be comfort-corrected.
            const Vec3 nativeCamera=s.raw.F;
            const Vec3 nativeBullet=unit(Add(nativeCamera,{.003f,-.001f,.002f}));
            const Vec3 nativeDeviation=Subtract(nativeBullet,nativeCamera);
            const Vec3 finalBullet=unit(Add(actualAim,nativeDeviation));
            const Vec3 expected=unit(Add(gameplay.game.Direction(refAim),nativeDeviation));
            Near(finalBullet,expected,"native ballistic deviation survives view-only cancellation");
            if(step) Check(DotProduct(Subtract(actualAim,visibleAim),Subtract(actualAim,visibleAim))>.0001f,
                "negative control: visual aim would incorrectly suppress gameplay recoil");

            // arms::Apply works around a local world origin. Both bases must
            // subtract the same O, and the gameplay shot adds it exactly once.
            const Vec3 localOrigin{8,16,2};
            auto visualLocal=visual.game,gameplayLocal=gameplay.game;
            visualLocal.P=Subtract(visualLocal.P,localOrigin);
            gameplayLocal.P=Subtract(gameplayLocal.P,localOrigin);
            const Vec3 localShot=visualLocal.Position(refOrigin);
            const Vec3 remapped=Add(gameplayLocal.Position(visualLocal.ReferencePosition(localShot)),localOrigin);
            Near(remapped,actualOrigin,"local origin cancels in visual-to-gameplay remapping");
        }
    }
}
}
int main() {
    CapturedAndTurning();RecoveryAndContinuousFire();Boundaries();GameplayGeometry();
    std::printf("view comfort: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
