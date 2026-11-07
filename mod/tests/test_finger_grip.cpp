#include "common/finger_grip.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

struct Bone {uint32_t hash=0;int parent=-1;};
static void Check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    Bone native[107];
    // Actual first-person hierarchy: five three-joint digits on each wrist.
    constexpr uint32_t left[]={0x2951af9e,0xb058fe24,0xc75fceb2,0x708401f6,0xe98d504c,0x9e8a60da,0x07e83889,0x9ee16933,0xe9e659a5,0x1930e6cb,0x8039b771,0xf73e87e7,0x346aedf6,0xad63bc4c,0xda648cda};
    constexpr uint32_t right[]={0x14869815,0x8d8fc9af,0xfa88f939,0x3e8f89d2,0xa786d868,0xd081e8fe,0x7001c5d2,0xe9089468,0x9e0fa4fe,0x50687415,0xc96125af,0xbe661539,0x4d53367d,0xd45a67c7,0xa35d5751};
    for(int i=0;i<15;++i){native[57+i]={left[i],i%3?56+i:56};native[82+i]={right[i],i%3?81+i:81};}
    fc2vr::finger_grip::Mapping map;
    Check(fc2vr::finger_grip::Resolve(native,107,56,81,map),"observed hierarchy resolves");
    Check(map.bone[0][1]==94 && map.bone[3][1]==82 && map.bone[12][1]==91,"thumb/index/little correspondence, not index offsets");
    native[95].parent=81;
    Check(!fc2vr::finger_grip::Resolve(native,107,56,81,map),"reject detached thumb joint");native[95].parent=94;
    native[100]=native[94];
    Check(!fc2vr::finger_grip::Resolve(native,107,56,81,map),"reject ambiguous identity");native[100]={};
    native[96].hash=0;
    Check(!fc2vr::finger_grip::Resolve(native,107,56,81,map),"reject missing distal thumb");
    // A wrist-relative digit frame with a nonzero lateral displacement.
    const float pose[]={1,0,0,0,0,0,-1,0,0,1,0,0,.02f,.04f,-.03f,1};
    float mirrored[16],restored[16];
    Check(fc2vr::finger_grip::MirrorRelative(pose,mirrored),"finite digit pose");
    Check(mirrored[12]==pose[12] && mirrored[13]==pose[13] && mirrored[14]==-pose[14],"reflect local lateral displacement, preserve finger reach");
    const float determinant=mirrored[0]*(mirrored[5]*mirrored[10]-mirrored[6]*mirrored[9])-mirrored[1]*(mirrored[4]*mirrored[10]-mirrored[6]*mirrored[8])+mirrored[2]*(mirrored[4]*mirrored[9]-mirrored[5]*mirrored[8]);
    Check(std::fabs(determinant-1)<.00001f,"preserve rotation/mesh winding");
    Check(fc2vr::finger_grip::MirrorRelative(mirrored,restored),"reverse conversion");
    for(int i=0;i<16;++i)Check(restored[i]==pose[i],"involution");
    restored[5]=std::numeric_limits<float>::infinity();
    Check(!fc2vr::finger_grip::MirrorRelative(restored,mirrored),"reject invalid pose before publication");
    std::puts("PASS: native digit identity, hierarchy rejection, reach and proper rotation");
}
