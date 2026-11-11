#include "common/native_pistol_pose.h"
// Hash pairs and parent chains observed in the native 107-bone player capture.
// Never infer finger identity from array ordering (left thumb precedes index;
// right thumb follows the little finger). Unknown rigs retain native fingers.
struct PistolGripPose {
    int left[15]{};fc2vr::presentation::Matrix local[15]{};bool valid=false,native=false;
};
static bool CapturePistolGrip(Bone* bones,int n,int leftHand,int rightHand,PistolGripPose& out) {
    using namespace fc2vr::presentation;
    static const uint32_t hashes[15][2]={
        {0x708401f6,0x14869815},{0xe98d504c,0x8d8fc9af},{0x9e8a60da,0xfa88f939},
        {0x07e83889,0x3e8f89d2},{0x9ee16933,0xa786d868},{0xe9e659a5,0xd081e8fe},
        {0x1930e6cb,0x7001c5d2},{0x8039b771,0xe9089468},{0xf73e87e7,0x9e0fa4fe},
        {0x346aedf6,0x50687415},{0xad63bc4c,0xc96125af},{0xda648cda,0xbe661539},
        {0x2951af9e,0x4d53367d},{0xb058fe24,0xd45a67c7},{0xc75fceb2,0xa35d5751}};
    if(n<=0 || n>512 || leftHand<0 || rightHand<0 || leftHand>=n || rightHand>=n) return false;
    auto read=[&](int i) { Matrix m;memcpy(m.data(),bones[i].model,64);m[3]=m[7]=m[11]=0;m[15]=1;return m; };
    Matrix inverse[2];if(!Inverse(read(leftHand),inverse[0]) || !Inverse(read(rightHand),inverse[1])) return false;
    PistolGripPose pose;int previous[2]={leftHand,rightHand};
    Matrix relative[2][15];
    for(int finger=0;finger<15;++finger) {
        int ids[2]={-1,-1};
        for(int side=0;side<2;++side) {
            for(int b=0;b<n;++b) if(bones[b].hash==hashes[finger][side]) { if(ids[side]>=0) return false;ids[side]=b; }
            const int parent=finger%3?previous[side]:(side?rightHand:leftHand);
            if(ids[side]<0 || bones[ids[side]].parent!=parent) return false;
            previous[side]=ids[side];relative[side][finger]=Multiply(read(ids[side]),inverse[side]);
            if(!detail::Affine(relative[side][finger])) return false;
        }
        pose.left[finger]=ids[0];pose.local[finger]=MirrorFinger(relative[1][finger]);
        const auto position=[](const Matrix& m) { return Vec3{m[12],m[13],m[14]}; };
        if(finger%3==0) {
            const auto d=fc2vr::tracking::Subtract(position(relative[0][finger]),position(pose.local[finger]));
            if(fc2vr::tracking::DotProduct(d,d)>.002f*.002f) return false;
        } else {
            float lengths[2];
            for(int side=0;side<2;++side) {
                const auto d=fc2vr::tracking::Subtract(position(relative[side][finger]),position(relative[side][finger-1]));
                lengths[side]=std::sqrt(fc2vr::tracking::DotProduct(d,d));
            }
            if(lengths[0]<.005f || lengths[0]>.1f || std::fabs(lengths[0]-lengths[1])>.002f) return false;
        }
    }
    pose.valid=true;out=pose;return true;
}
static bool ApplyNativePistolGrip(const fc2vr::presentation::NativePistolPoses& poses,uint32_t key,
    const fc2vr::presentation::Matrix& mainHand,PistolGripPose& grip,fc2vr::presentation::Matrix& hand) {
    using namespace fc2vr::presentation;
    const auto* pose=poses.Find(key);
    if(!grip.valid || !pose || !NativePistolFits(*pose,grip.local)) return false;
    const auto placed=Multiply(pose->wrist,mainHand);
    if(!detail::Rigid(placed)) return false;
    for(unsigned f=0;f<15;++f) grip.local[f]=pose->fingers[f];
    hand=placed;grip.native=true;return true;
}
