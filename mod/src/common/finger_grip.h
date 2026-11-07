#pragma once
#include <cmath>
#include <cstdint>

namespace fc2vr::finger_grip {
// Paired digit identities observed on Steam Dunia SHA256 6353936a...862765.
// Left thumb precedes the other fingers; right thumb follows them. Bone-index
// offsets would pair the thumb with the index finger and deform the mesh.
constexpr uint32_t hashes[15][2]={
    {0x2951af9e,0x4d53367d},{0xb058fe24,0xd45a67c7},{0xc75fceb2,0xa35d5751},
    {0x708401f6,0x14869815},{0xe98d504c,0x8d8fc9af},{0x9e8a60da,0xfa88f939},
    {0x07e83889,0x3e8f89d2},{0x9ee16933,0xa786d868},{0xe9e659a5,0xd081e8fe},
    {0x1930e6cb,0x7001c5d2},{0x8039b771,0xe9089468},{0xf73e87e7,0x9e0fa4fe},
    {0x346aedf6,0x50687415},{0xad63bc4c,0xc96125af},{0xda648cda,0xbe661539}
};
struct Mapping {int bone[15][2]{};};
template<class Bone> bool Resolve(const Bone* bones,int count,int left,int right,Mapping& out){
    if(!bones || count<=0 || count>512 || left<0 || right<0 || left>=count || right>=count || left==right)return false;
    Mapping next;
    for(int digit=0;digit<15;++digit)for(int side=0;side<2;++side){
        int found=-1;
        for(int i=0;i<count;++i)if(bones[i].hash==hashes[digit][side]){if(found>=0)return false;found=i;}
        if(found<0 || found==left || found==right)return false;
        const int parent=digit%3?next.bone[digit-1][side]:(side?right:left);
        if(bones[found].parent!=parent)return false;
        next.bone[digit][side]=found;
    }
    out=next;return true;
}
// Both the wrist and each digit use the mirrored bone-Z basis. A reflection
// on each side preserves a proper rotation and the native finger lengths.
inline bool MirrorRelative(const float* right,float* left){
    for(int i=0;i<16;++i)if(!std::isfinite(right[i]))return false;
    for(int row=0;row<4;++row)for(int col=0;col<4;++col)
        left[row*4+col]=right[row*4+col]*((row==2?-1:1)*(col==2?-1:1));
    return true;
}
}
