#include "common/support_pose.h"
#include "common/palm_anchor.h"
#include <cstdio>
using namespace fc2vr::presentation;
using namespace fc2vr::tracking;
static int checks=0,failures=0;
static void Check(bool pass,const char* name) { ++checks;if(!pass) { ++failures;std::fprintf(stderr,"FAIL: %s\n",name); } }
static bool Near(Vec3 a,Vec3 b) { auto d=Subtract(a,b);return DotProduct(d,d)<1e-10f; }
static Vec3 Position(const Matrix& m) { return {m[12],m[13],m[14]}; }
static Matrix Turn(float yaw,float pitch,Vec3 p={}) {
    auto y=Identity(),x=Identity();y[0]=y[5]=std::cos(yaw);y[1]=std::sin(yaw);y[4]=-y[1];
    x[5]=x[10]=std::cos(pitch);x[6]=std::sin(pitch);x[9]=-x[6];
    auto m=Multiply(x,y);m[12]=p.x;m[13]=p.y;m[14]=p.z;return m;
}
int main() {
    // Captured FC2 long-finger roots, mirrored for the left hand. Contact must
    // use the 43.5 mm anatomical palm, not the wrist joint or a guessed index.
    const Vec3 fingers[]={{.0851f,-.0054f,-.0382f},{.0881f,-.0055f,-.0111f},
        {.0903f,-.0050f,.0149f},{.0838f,-.0014f,.0390f}};
    Vec3 palm{};Check(PalmOffset(fingers,4,palm),"measured left palm available");
    const Vec3 mainPalm{.3f,.5f,1.2f},mainWrist{.3f,.4565875f,1.2021625f};
    const auto point=PistolSupportPoint(mainPalm,{1,0,0},{0,0,1},{0,-1,0});
    Check(Near(point,{.28f,.52f,1.13f}),"pistol contact is below main palm in controller frame");
    const auto legacy=PistolSupportPoint(mainWrist,{1,0,0},{0,0,1},{0,-1,0});
    Check(Near(Subtract(point,legacy),Subtract(mainPalm,mainWrist)),"right wrist displacement is not inherited by support contact");
    auto native=Turn(.3f,.4f,legacy);
    Check(!Near(PosePoint(native,palm),point),"negative control reproduces wrist-based support mismatch");
    for(float yaw:{-.8f,-.3f,0.f,.3f,.8f}) for(float pitch:{-.5f,0.f,.5f}) {
        auto hand=Turn(yaw,pitch,legacy),placed=Identity();
        Check(PlaceSupportPalm(hand,palm,point,placed),"pistol support palm can be seated");
        Check(Near(PosePoint(placed,palm),point),"support palm reaches exact contact under wrist rotation");
        for(unsigned i=0;i<12;++i) Check(placed[i]==hand[i],"saved hand orientation is unchanged");
        auto twice=Identity();Check(PlaceSupportPalm(placed,palm,point,twice)&&Near(Position(twice),Position(placed)),"seating twice does not accumulate");
        // Rigid whole-body turns must carry the same contacts and local angles.
        auto motion=Turn(.2f,-.1f,{.02f,-.03f,.01f});Matrix moved{};
        Check(PlaceSupportPalm(Multiply(hand,motion),palm,PosePoint(motion,point),moved),"common turn is valid");
        const auto expected=Multiply(placed,motion);
        Check(Near(Position(moved),Position(expected)),"physical/stick motion commutes with support placement");
    }
    // The existing late presenter must rotate around the seated support PALM.
    // Independent left wrist motion may not lift its palm off the pistol.
    auto right=Turn(.2f,0,mainPalm),left=Turn(-.3f,.2f,{-.2f,.4f,1.1f});
    const Vec3 contactLocal{-.02f,-.07f,-.02f};const auto contact=PosePoint(right,contactLocal);
    Matrix sourceHand{};Check(PlaceSupportPalm(left,palm,contact,sourceHand),"source solve seats palm");
    SupportHold hold;Check(CaptureSupport(right,mainPalm,contact,SupportMode::Pistol,hold),"late source captures palm contact");
    Matrix sourceRight{},sourceLeft{};Check(SupportedControllers(right,left,hold,sourceRight,sourceLeft),"source coupled frames valid");
    for(float angle:{-.35f,0.f,.35f}) {
        auto nowRight=Multiply(right,Turn(angle,.1f,{.02f,0,0}));
        auto nowLeft=Multiply(left,Turn(-angle,.15f,{.2f,.2f,0}));
        Matrix currentRight{},currentLeft{},delta{};
        Check(SupportedControllers(nowRight,nowLeft,hold,currentRight,currentLeft),"current pistol frames valid");
        Check(Delta(sourceLeft,currentLeft,Identity(),delta),"bounded support delta valid");
        Check(Near(PosePoint(Multiply(sourceHand,delta),palm),PosePoint(nowRight,contactLocal)),"late correction preserves palm contact with independent left rotation");
    }
    Matrix sentinel=Turn(.2f,.3f,{5,6,7}),out=sentinel;
    Check(!PlaceSupportPalm(native,{1,0,0},point,out)&&out==sentinel,"invalid anatomical offset preserves fallback");
    auto bad=native;bad[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!PlaceSupportPalm(bad,palm,point,out)&&out==sentinel,"nonfinite hand cannot publish partial correction");
    std::printf("pistol support: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
