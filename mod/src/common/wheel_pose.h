#pragma once

namespace fc2vr::presentation {
// A held wheel hand belongs to the native driving animation. Preserve its whole
// clavicle subtree, including finger/twist bones and proven external followers.
// Unknown/cyclic hierarchies keep the existing VR path. Camera/torso are never
// claimed. The caller publishes the unheld bones and leaves held model bytes as
// received from the native hierarchy pass; no inverse/recomposition is needed.
template<class Bone>
bool NativeWheelBone(const Bone* bones,int count,int bone,int camera,const int* follow,
                     int leftClavicle,int rightClavicle,unsigned held) {
    if (!(held&3u) || !bones || count<=0 || count>512 || bone<0 || bone>=count
        || bone==camera || leftClavicle<0 || leftClavicle>=count
        || rightClavicle<0 || rightClavicle>=count || leftClavicle==rightClavicle) return false;
    int node=bone;
    if(follow) {
        if(follow[bone]<-1 || follow[bone]>=count) return false;
        if(follow[bone]>=0) node=follow[bone];
    }
    unsigned side=0;
    for(int hops=0;node>=0;++hops) {
        if(node>=count || hops>=count || node==camera) return false;
        if(node==leftClavicle) side|=1u;
        if(node==rightClavicle) side|=2u;
        node=bones[node].parent;
        if(node<-1) return false;
    }
    return (side==1u || side==2u) && (side&held)!=0;
}
}
