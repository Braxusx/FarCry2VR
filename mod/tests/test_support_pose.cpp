#include "common/support_pose.h"
#include <cstdio>
using namespace fc2vr::presentation;
static int checks=0,failures=0;
static void Check(bool ok,const char* label) { ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",label); } }
static void Near(const Matrix& a,const Matrix& b,const char* label) {
    for(unsigned i=0;i<16;++i) Check(std::isfinite(a[i])&&std::fabs(a[i]-b[i])<2e-5f,label);
}
static Matrix Turn(float yaw,float pitch,Vec3 p={}) {
    auto y=Identity(),x=Identity();y[0]=y[5]=std::cos(yaw);y[1]=std::sin(yaw);y[4]=-y[1];
    x[5]=x[10]=std::cos(pitch);x[6]=std::sin(pitch);x[9]=-x[6];
    auto m=Multiply(x,y);m[12]=p.x;m[13]=p.y;m[14]=p.z;return m;
}
int main() {
    auto right=Identity(),left=Identity();left[13]=.4f;
    SupportHold rifle;Check(CaptureSupport(right,{},{0,.4f,0},SupportMode::Rifle,rifle),"capture pre-steer native foregrip");
    Matrix r{},l{};Check(SupportedControllers(right,left,rifle,r,l),"straight rifle span valid");
    Near(r,right,"straight span preserves orientation");Near(l,r,"rifle arms share one constraint");
    // Known 30-degree turn of the left controller about the right grip. The
    // weapon must steer 30 degrees even though the right controller is still.
    const auto turn=Turn(.5235987756f,0);left=Multiply(left,turn);
    Check(SupportedControllers(right,left,rifle,r,l),"support movement steers rifle");
    Near(r,turn,"rifle shortest-arc known-axis oracle");Near(l,r,"support stays on weapon after steering");
    Matrix delta{};Check(Delta(Identity(),r,Identity(),delta),"supported delta valid");Near(delta,turn,"render delta follows support steering");
    // Physical turning moves both controllers in reference space; stick turning
    // rotates the game basis. Either common transform must carry the same hold.
    for(float yaw:{-.4f,0.f,.35f}) for(float pitch:{-.25f,0.f,.2f}) {
        const auto motion=Turn(yaw,pitch,{.04f,-.02f,.01f});
        Matrix nr{},nl{};
        Check(SupportedControllers(Multiply(right,motion),Multiply(left,motion),rifle,nr,nl),"common turn/step keeps supported frame valid");
        Near(nr,Multiply(r,motion),"common physical or joystick transform commutes with rifle solve");
        Near(nl,nr,"two hands cannot separate under common movement");
        Check(Delta(r,nr,Identity(),delta),"common turn produces bounded render correction");
        Near(Multiply(r,delta),nr,"current supported target reached exactly");
    }
    // Nonzero palm pivot/custom hand offset must not orbit the controller.
    SupportHold custom{SupportMode::Rifle,{.01f,.02f,-.03f},{.01f,.42f,-.03f}};
    left=Identity();left[12]=.01f;left[13]=.42f;left[14]=-.03f;
    Check(SupportedControllers(right,left,custom,r,l),"nonzero native palm pivot supported");
    auto expected=Identity();expected[12]=.01f;expected[13]=.02f;expected[14]=-.03f;Near(r,expected,"pivot remains at native contact point");
    const auto original=custom;Matrix captureBasis=Turn(.3f,.2f,{.3f,.4f,1.2f});SupportHold captured;
    Check(CaptureSupport(captureBasis,PosePoint(captureBasis,custom.pivot),PosePoint(captureBasis,custom.foregrip),SupportMode::Rifle,captured),"capture in rotated controller frame");
    for(int frame=0;frame<50;++frame) {
        Check(SupportedControllers(right,left,captured,r,l),"repeated source valid");Near(r,expected,"support correction never accumulates");
    }
    Check(custom.pivot.x==original.pivot.x&&custom.foregrip.y==original.foregrip.y,"native source constraint unchanged");
    SupportHold pistol{SupportMode::Pistol,{},{-.02f,-.07f,.02f}};
    left=Turn(-.2f,.1f,{9,9,9});right=Turn(.1f,0,{.1f,.2f,.3f});
    Check(SupportedControllers(right,left,pistol,r,l),"pistol support valid");Near(r,right,"pistol does not steer weapon");
    expected=left;const auto point=PosePoint(right,pistol.foregrip);expected[12]=point.x;expected[13]=point.y;expected[14]=point.z;
    Near(l,expected,"pistol support point rides gun while retaining left wrist orientation");
    Matrix sentinel=Turn(.1f,.1f,{7,8,9});r=l=sentinel;right=left=Identity();
    Check(!SupportedControllers(right,left,rifle,r,l),"coincident rifle controllers rejected");Near(r,sentinel,"failure preserves caller output");
    left[13]=-.4f;Check(!SupportedControllers(right,left,rifle,r,l),"ambiguous opposite rifle span rejected");
    left[13]=3;Check(!SupportedControllers(right,left,rifle,r,l),"implausibly large span rejected");
    left[13]=.4f;right[0]=std::numeric_limits<float>::quiet_NaN();Check(!SupportedControllers(right,left,rifle,r,l),"invalid controller rejected");
    right=Identity();right[0]=2;Check(!CaptureSupport(right,{},{0,.4f,0},SupportMode::Rifle,captured),"scaled controller axes rejected");
    std::printf("support pose: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
