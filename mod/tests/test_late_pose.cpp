#include "common/late_pose.h"
#include <cstdio>
#include <limits>

using namespace fc2vr::presentation;
namespace {
int checks=0,failures=0;
constexpr float pi=3.14159265358979323846f;
void Check(bool ok,const char* message) { ++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",message);} }
void Near(float a,float b,const char* message,float tolerance=2e-5f) { Check(std::isfinite(a)&&std::fabs(a-b)<tolerance,message); }
void Near(Vec3 a,Vec3 b,const char* message,float tolerance=2e-5f) { Near(a.x,b.x,message,tolerance);Near(a.y,b.y,message,tolerance);Near(a.z,b.z,message,tolerance); }
void Near(const Matrix& a,const Matrix& b,const char* message,float tolerance=2e-5f) { for(unsigned i=0;i<16;++i)Near(a[i],b[i],message,tolerance); }
Matrix Yaw(float angle,Vec3 position={}) {
    Matrix result=Identity();const float c=std::cos(angle),s=std::sin(angle);
    result[0]=c;result[1]=s;result[4]=-s;result[5]=c;
    result[12]=position.x;result[13]=position.y;result[14]=position.z;return result;
}
Vec3 Point(Vec3 point,const Matrix& matrix) {
    const double p[3]={point.x,point.y,point.z};double out[3]={matrix[12],matrix[13],matrix[14]};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)out[i]+=p[j]*matrix[j*4+i];
    return {static_cast<float>(out[0]),static_cast<float>(out[1]),static_cast<float>(out[2])};
}
// Independent rigid-pose oracle: dot into the old controller's unit axes, then
// expand along the new controller axes. It does not use inverse/multiply helpers.
Vec3 Follow(Vec3 world,const Matrix& oldPose,const Matrix& newPose) {
    const double d[3]={double(world.x)-oldPose[12],double(world.y)-oldPose[13],double(world.z)-oldPose[14]};
    double local[3]{},result[3]={newPose[12],newPose[13],newPose[14]};
    for(unsigned i=0;i<3;++i)for(unsigned k=0;k<3;++k)local[i]+=d[k]*oldPose[i*4+k];
    for(unsigned i=0;i<3;++i)for(unsigned k=0;k<3;++k)result[k]+=local[i]*newPose[i*4+k];
    return {static_cast<float>(result[0]),static_cast<float>(result[1]),static_cast<float>(result[2])};
}

void IndependentHandsAndScale() {
    auto pose=[](float yaw,float pitch,float roll,Vec3 position) {
        Matrix x=Identity(),y=Identity();
        x[5]=x[10]=std::cos(pitch);x[6]=std::sin(pitch);x[9]=-x[6];
        y[0]=y[10]=std::cos(roll);y[2]=-std::sin(roll);y[8]=-y[2];
        auto result=Multiply(Multiply(x,y),Yaw(yaw));
        result[12]=position.x;result[13]=position.y;result[14]=position.z;return result;
    };
    const Matrix old[2]={pose(-.2f,.1f,-.15f,{-.3f,.4f,1.2f}),pose(.3f,-.1f,.2f,{.3f,.5f,1.3f})};
    const Matrix current[2]={pose(.25f,.3f,-.1f,{-.27f,.46f,1.18f}),pose(.1f,-.25f,-.1f,{.35f,.48f,1.33f})};
    Matrix root=pose(.6f,.2f,-.1f,{});for(unsigned c=0;c<3;++c) {root[c]*=.01f;root[4+c]*=.02f;root[8+c]*=.015f;}
    const Matrix bones[2]={Yaw(.1f,{-28,35,75}),Yaw(-.1f,{25,30,80})};
    Matrix bind=Identity();bind[0]=.8f;bind[5]=1.1f;bind[10]=1.2f;bind[12]=.3f;bind[13]=-.1f;
    const Vec3 vertex{.2f,.1f,-.3f};
    Matrix deltas[2];
    for(unsigned hand=0;hand<2;++hand) {
        Check(Delta(old[hand],current[hand],root,deltas[hand]),"independent hand correction accepts scaled root");
        Packed source{},corrected{};Check(Pack(Multiply(bind,bones[hand]),source),"pack native skin source");
        Check(ApplyPacked(source,deltas[hand],corrected),"apply model-space hand correction");
        const auto nativeWorld=Point(Point(Point(vertex,bind),bones[hand]),root);
        const auto expected=Follow(nativeWorld,old[hand],current[hand]);
        const auto actual=Point(Point(vertex,Unpack(corrected)),root);
        Near(actual,expected,"corrected skin follows independent rigid controller oracle");
        Packed secondEye{};Check(ApplyPacked(source,deltas[hand],secondEye),"same source supports second eye");
        Check(secondEye==corrected,"both eyes receive identical corrected skin");
        const auto affine=Unpack(corrected);Near(affine[3],0,"affine last column x");Near(affine[7],0,"affine last column y");Near(affine[11],0,"affine last column z");Near(affine[15],1,"affine homogeneous coordinate");
    }
    const Vec3 modelPoint{10,20,30};
    const auto left=Point(modelPoint,deltas[0]),right=Point(modelPoint,deltas[1]);
    Check(std::fabs(left.x-right.x)+std::fabs(left.y-right.y)>.1f,"fixture distinguishes the two hand corrections");
}

void PackedLayoutAndRepeatedSource() {
    const Packed known={1,4,7,10,2,5,8,11,3,6,9,12};
    const Matrix expected={1,2,3,0,4,5,6,0,7,8,9,0,10,11,12,1};
    Near(Unpack(known),expected,"native transposed 48-byte layout");
    Packed roundTrip{};Check(Pack(expected,roundTrip)&&roundTrip==known,"packed layout round trip");
    const auto old=Yaw(.1f,{.3f,.2f,1}),one=Yaw(.2f,{.32f,.21f,1}),two=Yaw(.3f,{.35f,.24f,1.02f});
    Matrix d1,d2;Check(Delta(old,one,Identity(),d1),"first correction");Check(Delta(old,two,Identity(),d2),"recomputed correction");
    Packed source{};Check(Pack(Yaw(-.2f,{.25f,.3f,1.1f}),source),"source packed");
    const auto original=source;
    struct Guarded { float before=123;Packed data{};float after=456; } first,second;
    Check(ApplyPacked(source,d1,first.data),"first destination copy");
    Check(ApplyPacked(source,d2,second.data),"new destination recomputed from original source");
    Check(source==original,"original native source untouched");Near(first.before,123,"prefix guard");Near(first.after,456,"suffix guard");
    const Vec3 point{.01f,.02f,.03f};
    Near(Point(point,Unpack(second.data)),Follow(Point(point,Unpack(source)),old,two),"repeated source uses total old-to-current correction once");
    Packed incorrect{};Check(ApplyPacked(first.data,d2,incorrect),"negative control accumulates two corrections");
    const auto wrong=Point(point,Unpack(incorrect)),right=Point(point,Unpack(second.data));
    Check(std::fabs(wrong.x-right.x)+std::fabs(wrong.y-right.y)>.01f,"negative control exposes accumulated-delta drift");
}

void PrecisionAndInverse() {
    GameBasis game;game.P={3000.125f,-4000.25f,100.5f};const auto root=game.P;
    const Vec3 right{1,0,0},up{0,1,0},back{0,0,1};
    const auto old=Controller(game,right,up,back,{.123456f,.2f,-.4f},root);
    const auto next=Controller(game,right,up,back,{.123556f,.2f,-.4f},root);
    Near(old[12],.123456f,"large world origin cancels before float narrowing",1e-7f);
    Near(next[12]-old[12],.0001f,"sub-millimetre controller motion survives",2e-8f);
    Matrix delta;Check(Delta(old,next,Identity(),delta),"small tracking correction accepted");Near(delta[12],.0001f,"small delta retained",2e-8f);
    const float naiveOld=game.P.x+.123456f,naiveNew=game.P.x+.123556f;
    Check(std::fabs((naiveNew-naiveOld)-.0001f)>.00005f,"negative control loses motion when world origin is added first");

    Matrix affine=Yaw(.8f,{2,3,4});affine[0]*=2;affine[1]*=2;affine[4]*=.5f;affine[5]*=.5f;
    Matrix inverse;Check(Inverse(affine,inverse),"affine inverse with scale");Near(Multiply(affine,inverse),Identity(),"affine inverse product");
    auto general=affine;general[3]=.1f;general[7]=-.03f;general[11]=.02f;
    Check(Inverse(general,inverse),"general projective inverse");Near(Multiply(general,inverse),Identity(),"general inverse product");
}

void InvalidsAndBounds() {
    const auto sentinel=Yaw(.2f,{7,8,9});Matrix output=sentinel;
    Matrix zero{};Check(!Inverse(zero,output),"singular inverse rejected");Check(output==sentinel,"failed inverse preserves destination");
    const auto old=Yaw(0,{.2f,.3f,1});
    Check(!Delta(old,Yaw(0,{.701f,.3f,1}),Identity(),output),"grip jump above half metre rejected");
    Check(output==sentinel,"failed delta preserves destination");
    Check(!Delta(old,Yaw(61*pi/180,{.2f,.3f,1}),Identity(),output),"rotation above sixty degrees rejected");
    Check(Delta(old,Yaw(59*pi/180,{.2f,.3f,1}),Identity(),output),"large valid rotation around stationary grip accepted");
    Check(!Delta(old,old,zero,output),"singular root rejected");
    Check(!Delta(old,old,Yaw(0,{3000,0,0}),output),"absolute world root translation rejected");
    auto bad=old;bad[0]=std::numeric_limits<float>::quiet_NaN();Check(!Delta(bad,old,Identity(),output),"nonfinite controller rejected");
    bad=old;bad[0]=.8f;Check(!Delta(old,bad,Identity(),output),"controller scaling is not accepted as tracking rotation");
    Packed source{},destination{};destination.fill(91);const auto saved=destination;
    source[0]=std::numeric_limits<float>::infinity();Check(!ApplyPacked(source,Identity(),destination),"nonfinite skin rejected");Check(destination==saved,"failed skin correction preserves destination");
    source.fill(0);bad=Identity();bad[3]=.1f;Check(!ApplyPacked(source,bad,destination),"projective skin correction rejected");
    GameBasis game;game.P.x=std::numeric_limits<float>::quiet_NaN();
    bad=Controller(game,{1,0,0},{0,1,0},{0,0,1},{},{ });
    Check(!Delta(bad,old,Identity(),output),"invalid controller construction fails closed downstream");
}
}
int main() {
    IndependentHandsAndScale();PackedLayoutAndRepeatedSource();PrecisionAndInverse();InvalidsAndBounds();
    std::printf("late pose: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
