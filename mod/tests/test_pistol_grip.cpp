#include "common/pistol_grip.h"
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>
#include <fstream>
using namespace fc2vr::presentation;
using namespace fc2vr::tracking;
struct Bone { uint32_t hash;int parent;float model[16]; };
#include "arms_pistol_grip.inl"
static int checks=0,failures=0;
static void Check(bool ok,const char* msg) { ++checks;if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",msg); } }
static Vec3 Pos(const Matrix& m) { return {m[12],m[13],m[14]}; }
static bool Near(Vec3 a,Vec3 b) { const auto d=Subtract(a,b);return DotProduct(d,d)<1e-9f; }
static Matrix Read(const Bone& b) { Matrix m;std::memcpy(m.data(),b.model,64);return m; }
static Matrix Turn(float angle,Vec3 p={}) {
    auto m=Identity();m[0]=m[5]=std::cos(angle);m[1]=std::sin(angle);m[4]=-m[1];
    m[12]=p.x;m[13]=p.y;m[14]=p.z;return m;
}
static std::array<Bone,32> ProceduralRig() {
    std::array<Bone,32> rig{};
    static const uint32_t hashes[15][2]={
        {0x708401f6,0x14869815},{0xe98d504c,0x8d8fc9af},{0x9e8a60da,0xfa88f939},
        {0x07e83889,0x3e8f89d2},{0x9ee16933,0xa786d868},{0xe9e659a5,0xd081e8fe},
        {0x1930e6cb,0x7001c5d2},{0x8039b771,0xe9089468},{0xf73e87e7,0x9e0fa4fe},
        {0x346aedf6,0x50687415},{0xad63bc4c,0xc96125af},{0xda648cda,0xbe661539},
        {0x2951af9e,0x4d53367d},{0xb058fe24,0xd45a67c7},{0xc75fceb2,0xa35d5751}};
    for(int side=0;side<2;++side) {
        const int wrist=side*16;rig[wrist].parent=-1;
        const auto identity=Identity();memcpy(rig[wrist].model,identity.data(),64);
        for(int finger=0;finger<15;++finger) {
            const int bone=wrist+finger+1;auto m=Identity();
            m[12]=.02f+.02f*(finger%3);m[13]=.012f*(finger/3);m[14]=.003f*(finger/3);
            if(!side) m=MirrorFinger(m);
            rig[bone].hash=hashes[finger][side];rig[bone].parent=finger%3?bone-1:wrist;
            memcpy(rig[bone].model,m.data(),64);
        }
    }
    return rig;
}
int main() {
    const auto captured=ProceduralRig();
    Bone bones[32];std::memcpy(bones,captured.data(),sizeof bones);
    PistolGripPose pose;
    Check(CapturePistolGrip(bones,int(std::size(bones)),0,16,pose)&&pose.valid,"procedural bilateral rig accepts native grip transfer");
    Check(std::memcmp(bones,captured.data(),sizeof bones)==0,"capture never mutates native animation or local bones");
    Matrix inverse;Check(Inverse(Read(bones[16]),inverse),"right wrist inverse available");
    for(int finger=0;finger<15;++finger) {
        const int right=finger+17;
        const auto local=Multiply(Read(bones[right]),inverse);
        Check(Near(Pos(pose.local[finger]),{local[12],local[13],-local[14]}),"index through thumb use correct mirrored native bone");
        Check(detail::Rigid(pose.local[finger]),"mirroring preserves positive rigid skin basis");
        Check(MirrorFinger(MirrorFinger(local))==local,"finger reflection is involutive");
    }
    const auto native=pose;
    // Reordering
    // the entire array must keep the same mapping, without assumed bone indices.
    Bone shuffled[32];const int count=int(std::size(captured));
    for(int i=0;i<count;++i) { shuffled[count-1-i]=bones[i];shuffled[count-1-i].parent=bones[i].parent<0?-1:count-1-bones[i].parent; }
    Check(CapturePistolGrip(shuffled,count,count-1,count-17,pose),"reordered rig maps by hashes and verified parents");
    for(int f=0;f<15;++f) Check(pose.local[f]==native.local[f] && pose.left[f]==count-1-native.left[f],"reordering does not swap thumb/index or finger segments");
    for(int invalid=0;invalid<4;++invalid) {
        std::memcpy(bones,captured.data(),sizeof bones);pose=native;
        if(invalid==0) bones[1].hash=0;
        if(invalid==1) bones[2].parent=0;
        if(invalid==2) bones[4].model[12]+=.02f;
        if(invalid==3) bones[19].model[5]=std::numeric_limits<float>::quiet_NaN();
        Check(!CapturePistolGrip(bones,count,0,16,pose),"unknown or distorted finger topology fails closed");
        Check(std::memcmp(&pose,&native,sizeof pose)==0,"failed capture leaves destination untouched");
    }
    const Vec3 palm{.04f,-.002f,.001f};
    for(float angle:{-.4f,0.f,.4f}) {
        const auto right=Turn(angle,{.3f,.4f,1.3f});const auto contact=PistolGripContact(right,palm);Matrix hand;
        Check(Near(contact,PosePoint(right,{palm.x,palm.y+.055f,palm.z})),"55 mm palm separation follows the gun normal through turns");
        Check(PistolGripHand(right,palm,contact,hand),"opposed support grip can be seated");
        Check(Near(PosePoint(hand,palm),contact),"support palm reaches the opposed grip contact");
        Check(Near({hand[0],hand[1],hand[2]},{right[0],right[1],right[2]}),"fingers point along the main hand");
        Check(Near({hand[4],hand[5],hand[6]},{-right[4],-right[5],-right[6]}),"support palm faces main palm");
        SupportHold hold;Check(CaptureSupport(right,PosePoint(right,palm),contact,SupportMode::PistolGrip,hold),"capture grip constraint");
        Matrix r0,l0,r1,l1,delta;const auto left=Turn(-.5f,{.2f,.8f,1});
        Check(SupportedControllers(right,left,hold,r0,l0),"source grip controller frames");
        const auto motion=Turn(.2f,{.02f,-.01f,.03f}),next=Multiply(right,motion);
        Check(SupportedControllers(next,Turn(.9f,{4,5,6}),hold,r1,l1),"left controller cannot peel supported hand off pistol");
        Check(Delta(l0,l1,Identity(),delta),"bounded grip delta valid");
        const auto moved=Multiply(hand,delta),expected=Multiply(hand,motion);
        for(unsigned i=0;i<16;++i) Check(std::fabs(moved[i]-expected[i])<2e-5f,"full hand and grip pose follow gun motion");
    }
    // Exercise the actual file parser and production application seam with
    // synthetic data; no proprietary animation payload is stored in Git.
    NativePistolPoses table;table.count=1;
    auto& entry=table.entries[0];entry.weaponKey=0x12345678;entry.wrist=Turn(.6f,{.01f,.06f,-.02f});
    for(unsigned f=0;f<15;++f) entry.fingers[f]=native.local[f];
    std::vector<uint8_t> bytes;
    auto word=[&](uint32_t w) { for(unsigned b=0;b<4;++b) bytes.push_back(uint8_t(w>>(8*b))); };
    word(0x504e3246);word(1);word(1);word(entry.weaponKey);
    for(unsigned m=0;m<16;++m) for(float v:m?entry.fingers[m-1]:entry.wrist) { uint32_t bits;std::memcpy(&bits,&v,4);word(bits); }
    NativePistolPoses parsed;
    Check(ParseNativePistolPoses(bytes.data(),bytes.size(),parsed),"valid private pose payload accepted");
    Check(parsed.count==1 && parsed.Find(entry.weaponKey),"file routes by exact weapon identity");
    Check(!parsed.Find(0) && !parsed.Find(0xffffffff) && !parsed.Find(2),"unknown and unsettled weapon identities do not select a pose");
    for(size_t size=0;size<bytes.size();++size)
        Check(!ParseNativePistolPoses(bytes.data(),size,parsed),"every truncated payload rejected");
    for(unsigned mutation=0;mutation<7;++mutation) {
        auto bad=bytes;
        if(mutation==0) bad[0]=0;
        if(mutation==1) bad[4]=2;
        if(mutation==2) bad[8]=9;
        if(mutation==3) { for(unsigned b=12;b<16;++b) bad[b]=0; }
        if(mutation==4) { uint32_t nan=0x7fc00000;std::memcpy(bad.data()+16,&nan,4); }
        if(mutation==5) { float scale=3;std::memcpy(bad.data()+16,&scale,4); }
        if(mutation==6) bad.push_back(0);
        const auto before=parsed;
        Check(!ParseNativePistolPoses(bad.data(),bad.size(),parsed),"malformed private pose data rejected");
        Check(std::memcmp(&before,&parsed,sizeof parsed)==0,"rejected file leaves pose table unchanged");
    }
    auto duplicate=bytes;duplicate[8]=2;duplicate.insert(duplicate.end(),bytes.begin()+12,bytes.end());
    Check(!ParseNativePistolPoses(duplicate.data(),duplicate.size(),parsed),"duplicate weapon keys rejected");
    for(float angle:{-.5f,0.f,.5f}) {
        const auto right=Turn(angle,{.2f,-.3f,1});Matrix hand=Identity();auto grip=native;
        Check(ApplyNativePistolGrip(parsed,entry.weaponKey,right,grip,hand) && grip.native,"verified authored grip applied");
        Check(hand==Multiply(entry.wrist,right),"authored wrist relation follows weapon hand through turns");
        for(unsigned f=0;f<15;++f) Check(grip.local[f]==entry.fingers[f] && grip.left[f]==native.left[f],"authored fingers keep verified live bone routing");
        auto bad=native;bad.local[0][12]+=.01f;
        Check(!ApplyNativePistolGrip(parsed,entry.weaponKey,right,bad,hand) && !bad.native,"wrong hand anatomy falls back");
        Check(!ApplyNativePistolGrip(parsed,2,right,bad,hand),"unknown weapon cannot inherit prior weapon pose");
    }
    std::printf("pistol grip: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
