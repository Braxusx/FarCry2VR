#pragma once
namespace fc2vr::left_attachment {
enum class Offset { Reject,Capture,Reuse };
// Keep the first native mount for this owned entity when FreezeReload is on.
// Never turn a converted offset or an animated reload offset into a new rest.
inline Offset NativeOffset(bool have,bool freeze,bool reloading,int index,int right,int left,bool stillOurs,bool noOffset){
    if(!have)return index==right && !stillOurs?Offset::Capture:Offset::Reject;
    if(index==right)return freeze || stillOurs?Offset::Reuse:Offset::Capture;
    if(index==left)return Offset::Reuse;
    return freeze && (reloading || noOffset)?Offset::Reuse:Offset::Reject;
}
}
