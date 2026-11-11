#pragma once
#include "pistol_grip.h"
#include <cstdint>
#include <cstring>

namespace fc2vr::presentation {
// Private, locally decoded animation data. No game assets are embedded in the
// mod. F2NP v1 is little-endian: magic/version/count, then key + 16 matrices.
// Matrices use native row vectors: left wrist relative to right wrist, followed
// by 15 left fingers relative to left wrist (index/middle/ring/little/thumb).
struct NativePistolPose {
    uint32_t weaponKey=0;
    Matrix wrist{};
    Matrix fingers[15]{};
};
struct NativePistolPoses {
    static constexpr size_t Capacity=8, RecordBytes=4+16*64, MaxBytes=12+Capacity*RecordBytes;
    NativePistolPose entries[Capacity]{};
    size_t count=0;
    const NativePistolPose* Find(uint32_t key) const {
        if(!key || key==0xffffffffu) return nullptr;
        for(size_t i=0;i<count;++i) if(entries[i].weaponKey==key) return &entries[i];
        return nullptr;
    }
};
inline float PoseDistance(const Matrix& a,const Matrix& b) {
    float square=0;for(unsigned i=12;i<15;++i) { const float d=a[i]-b[i];square+=d*d; }
    return std::sqrt(square);
}
inline bool ParseNativePistolPoses(const void* data,size_t size,NativePistolPoses& out) {
    if(!data || size<12 || size>NativePistolPoses::MaxBytes) return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&]() { const uint32_t v=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);p+=4;return v; };
    if(word()!=0x504e3246u || word()!=1) return false;
    const uint32_t count=word();
    if(!count || count>NativePistolPoses::Capacity || size!=12+count*NativePistolPoses::RecordBytes) return false;
    NativePistolPoses parsed;
    for(uint32_t i=0;i<count;++i) {
        auto& pose=parsed.entries[i];pose.weaponKey=word();
        if(!pose.weaponKey || pose.weaponKey==0xffffffffu || parsed.Find(pose.weaponKey)) return false;
        for(unsigned m=0;m<16;++m) {
            Matrix& matrix=m?pose.fingers[m-1]:pose.wrist;
            for(float& value:matrix) { const uint32_t bits=word();std::memcpy(&value,&bits,4); }
            if(!detail::Rigid(matrix) || PoseDistance(matrix,Identity())>.35f) return false;
            if(m && (m-1)%3) {
                const float length=PoseDistance(matrix,pose.fingers[m-2]);
                if(length<.005f || length>.1f) return false;
            }
        }
        ++parsed.count;
    }
    out=parsed;return true;
}
// Match the live rig's knuckles and segment lengths before substituting a pose.
// This comparison permits articulation but rejects a different hand anatomy.
inline bool NativePistolFits(const NativePistolPose& pose,const Matrix (&live)[15]) {
    for(unsigned f=0;f<15;++f) {
        if(!detail::Rigid(live[f])) return false;
        if(f%3==0) { if(PoseDistance(pose.fingers[f],live[f])>.002f) return false; }
        else if(std::fabs(PoseDistance(pose.fingers[f],pose.fingers[f-1])-PoseDistance(live[f],live[f-1]))>.002f) return false;
    }
    return true;
}
} // namespace fc2vr::presentation
