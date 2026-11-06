#pragma once

namespace fc2vr::hand_attachment {
// Row-vector convention, as used by Dunia. The attachment on the anatomical
// hand must produce the same weapon world transform as the calibrated grip.
inline void Compose(const float* base,const float* grip,const float* inverseHand,float* offset) {
    float world[16]{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)
        for(int k=0;k<4;++k)world[i*4+j]+=base[i*4+k]*grip[k*4+j];
    for(int i=0;i<4;++i)for(int j=0;j<4;++j) {
        float value=0;for(int k=0;k<4;++k)value+=world[i*4+k]*inverseHand[k*4+j];
        offset[i*4+j]=value;
    }
}
} // namespace fc2vr::hand_attachment
