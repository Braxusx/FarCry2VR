#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
namespace fc2vr::scenario {
inline std::uint64_t TopologyWord(std::uint64_t hash,std::uint32_t word) {
    for(unsigned i=0;i<4;++i) { hash^=static_cast<std::uint8_t>(word>>(8*i));hash*=1099511628211ull; }
    return hash;
}
enum class Pixels { Rgba8,Bgra8,Rgba16f,Rgba32f,Unsupported };
inline float Half(std::uint16_t h) {
    const unsigned e=(h>>10)&31,m=h&1023;
    if(e==31) return m?NAN:(h&0x8000?-INFINITY:INFINITY);
    const float f=e?std::ldexp(1.0f+float(m)/1024.0f,int(e)-15):std::ldexp(float(m),-24);
    return h&0x8000?-f:f;
}
inline void Put32(std::vector<std::uint8_t>& b,std::size_t i,std::uint32_t n) {
    for(unsigned k=0;k<4;++k) b[i+k]=static_cast<std::uint8_t>(n>>(8*k));
}
// Bounded encoder used by the D3D adapter and portable fixture tests. No GPU/file IO.
inline bool Bmp(const std::uint8_t* pixels,std::size_t bytes,unsigned width,unsigned height,
                std::size_t pitch,Pixels format,std::vector<std::uint8_t>& out) {
    out.clear();
    const unsigned bpp=format==Pixels::Rgba16f?8:format==Pixels::Rgba32f?16:4;
    if(!pixels || !width || !height || width>4096 || height>4096 || format==Pixels::Unsupported
        || pitch<std::size_t(width)*bpp || pitch>std::size_t(4096)*16 || bytes/pitch<height) return false;
    const std::size_t row=(std::size_t(width)*3+3)&~std::size_t(3);
    std::vector<std::uint8_t> b(54+row*height,0);
    b[0]='B';b[1]='M';Put32(b,2,static_cast<std::uint32_t>(b.size()));Put32(b,10,54);Put32(b,14,40);
    Put32(b,18,width);Put32(b,22,height);b[26]=1;b[28]=24;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        const auto* p=pixels+y*pitch+std::size_t(x)*bpp;
        auto* dst=b.data()+54+(height-1-y)*row+std::size_t(x)*3;
        if(format==Pixels::Rgba8 || format==Pixels::Bgra8) { dst[0]=p[format==Pixels::Bgra8?0:2];dst[1]=p[1];dst[2]=p[format==Pixels::Bgra8?2:0]; }
        else for(unsigned c=0;c<4;++c) {
            float f;
            if(format==Pixels::Rgba16f) { std::uint16_t h;std::memcpy(&h,p+2*c,2);f=Half(h); }
            else std::memcpy(&f,p+4*c,4);
            if(!std::isfinite(f)) return false;
            if(c<3) { f=f>0?f/(1+f):0;dst[2-c]=static_cast<std::uint8_t>(std::pow(f,1/2.2f)*255+0.5f); }
        }
    }
    out.swap(b);return true;
}
struct CaptureIdentity {
    std::uint64_t id=0,frame=0,sequence=0,owner=0,poseSequence=0,poseFrame=0;
    std::uint64_t generation=0,space=0,rig=0;
    std::int64_t displayTime=0;
    bool physical=false,named=false;
    unsigned format=0,width=0,height=0;
    bool operator==(const CaptureIdentity& b) const {
        return id==b.id&&frame==b.frame&&sequence==b.sequence&&owner==b.owner
            &&poseSequence==b.poseSequence&&poseFrame==b.poseFrame&&generation==b.generation
            &&space==b.space&&rig==b.rig&&displayTime==b.displayTime&&physical==b.physical&&named==b.named
            &&format==b.format&&width==b.width&&height==b.height;
    }
};
// A pair must also agree on the actual applied pose, rig and XR clock/context.
class EyePair {
    CaptureIdentity identity{};unsigned mask=0;
public:
    bool Has(unsigned eye,const CaptureIdentity& key) const {
        return eye<2&&identity==key&&(mask&(1u<<eye));
    }
    bool Add(unsigned eye,const CaptureIdentity& key,bool written) {
        if(eye>1 || !key.id || !key.frame || !key.sequence || !key.owner || !written) return false;
        if(!(identity==key)) { identity=key;mask=0; }
        mask|=1u<<eye;return mask==3;
    }
    void Clear() { identity={};mask=0; }
};
} // namespace fc2vr::scenario
