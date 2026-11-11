#include "common/wheel_pose.h"
#include <cstdio>
#include <cstring>
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* label) { if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",label); } };
    // Native wheel contact preserves a complete held arm, including its sibling
    // twists and root-parented sleeve helper. The other hand remains free in VR.
    struct PoseBone { int parent;unsigned model[16]; };
    PoseBone rig[19]{};
    const int parents[19]={-1,0,1,1,3,4,5,6,5,1,9,10,11,12,11,0,0,1,17};
    const unsigned side[19]={0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,1,2,0,0};
    int followers[19];
    for(int i=0;i<19;++i) {
        rig[i].parent=parents[i];followers[i]=-1;
        for(int j=0;j<16;++j) rig[i].model[j]=0x10000u+unsigned(i*16+j); // opaque/native bits included
    }
    followers[15]=5;followers[16]=11;
    const auto locked=[&](const PoseBone* bones,int count,int bone,unsigned held) {
        return fc2vr::presentation::NativeWheelBone(bones,count,bone,2,followers,3,9,held);
    };
    for(unsigned held=0;held<4;++held) {
        PoseBone posed[19];std::memcpy(posed,rig,sizeof rig);
        for(int i=0;i<19;++i) {
            const bool keep=locked(posed,19,i,held);
            check(keep==((side[i]&held)!=0),"held-side subtree includes twists/fingers/followers only");
            if(!keep && side[i]) for(auto& value:posed[i].model) value=0xFFFFFFFFu; // VR publication
        }
        for(int i=0;i<19;++i) {
            const bool nativeExpected=!side[i] || (side[i]&held)!=0;
            check((std::memcmp(posed[i].model,rig[i].model,sizeof rig[i].model)==0)==nativeExpected,
                "native held arm/camera/torso bytes preserved while released arm resumes VR");
        }
    }
    check(!locked(rig,19,-1,3) && !locked(rig,19,19,3),"invalid bone index cannot claim wheel pose");
    check(!locked(nullptr,19,3,3) && !locked(rig,513,3,3),"invalid rig cannot claim wheel pose");
    check(!fc2vr::presentation::NativeWheelBone(rig,19,3,2,followers,3,3,3),"ambiguous clavicles rejected");
    rig[4].parent=5; // 4 -> 5 -> 4 cycle
    check(!locked(rig,19,7,1),"cyclic ancestry cannot claim native contact");
    rig[4].parent=3;rig[3].parent=4; // even a cycle through the claimed clavicle is invalid
    check(!locked(rig,19,7,1),"clavicle-containing cycle rejected");
    rig[3].parent=1;rig[4].parent=99;
    check(!locked(rig,19,7,1),"out-of-range parent rejected");
    rig[4].parent=3;followers[15]=99;
    check(!locked(rig,19,15,1),"invalid external follower rejected");
    followers[15]=2;
    check(!locked(rig,19,15,3),"camera follower never claimed");
    std::printf("native wheel contact tests: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
