#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fc2vr::image {
using Digest=std::array<std::uint8_t,32>;
// Streaming SHA-256, with no provider/DLL loading from the proxy's loader-lock path.
class Sha256 {
    std::uint32_t h[8]{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::uint8_t block[64]{}; std::size_t used=0; std::uint64_t bytes=0; bool done=false,bad=false;
    Digest digest{};
    static std::uint32_t R(std::uint32_t x,unsigned n) { return (x>>n)|(x<<(32-n)); }
    void Compress() {
        constexpr std::uint32_t k[64]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for(int i=0;i<16;++i) w[i]=(std::uint32_t(block[4*i])<<24)|(std::uint32_t(block[4*i+1])<<16)|(std::uint32_t(block[4*i+2])<<8)|block[4*i+3];
        for(int i=16;i<64;++i) { const auto a=w[i-15],b=w[i-2]; w[i]=w[i-16]+(R(a,7)^R(a,18)^(a>>3))+w[i-7]+(R(b,17)^R(b,19)^(b>>10)); }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(int i=0;i<64;++i) {
            const auto t1=z+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));
            z=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=z;
    }
public:
    bool Update(const void* input,std::size_t size) {
        if(done || bad || (!input && size) || size>(UINT64_MAX/8)-bytes) { bad=true; return false; }
        bytes+=size; const auto* data=static_cast<const std::uint8_t*>(input);
        while(size) {
            const std::size_t n=size<64-used?size:64-used;
            std::memcpy(block+used,data,n); used+=n; data+=n; size-=n;
            if(used==64) { Compress(); used=0; }
        }
        return true;
    }
    bool Finish(Digest& out) {
        if(bad) return false;
        if(!done) {
            const std::uint64_t bits=bytes*8;
            block[used++]=0x80;
            if(used>56) { while(used<64) block[used++]=0; Compress(); used=0; }
            while(used<56) block[used++]=0;
            for(int i=0;i<8;++i) block[56+i]=static_cast<std::uint8_t>(bits>>(56-8*i));
            Compress();
            for(int i=0;i<32;++i) digest[i]=static_cast<std::uint8_t>(h[i/4]>>(24-8*(i%4)));
            done=true;
        }
        out=digest; return true;
    }
};
inline void Hex(const Digest& hash,char (&out)[65]) {
    constexpr char chars[]="0123456789abcdef";
    for(int i=0;i<32;++i) { out[2*i]=chars[hash[i]>>4]; out[2*i+1]=chars[hash[i]&15]; } out[64]=0;
}
inline bool ParseDigest(const char* text,Digest& out) {
    if(!text || std::strlen(text)!=64) return false;
    Digest candidate{};
    for(int i=0;i<64;++i) {
        const char c=text[i]; const int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(n<0) return false;
        candidate[i/2]|=static_cast<std::uint8_t>(n<<(i%2?0:4));
    }
    out=candidate; return true;
}
struct Pe {
    std::uint32_t timestamp=0,imageSize=0,imageBase=0,headersSize=0;
    std::uint16_t machine=0,magic=0,sections=0;
    bool operator==(const Pe& p) const { return timestamp==p.timestamp && imageSize==p.imageSize && imageBase==p.imageBase && headersSize==p.headersSize && machine==p.machine && magic==p.magic && sections==p.sections; }
};
inline std::uint16_t U16(const std::uint8_t* p) { return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8); }
inline std::uint32_t U32(const std::uint8_t* p) { return std::uint32_t(U16(p))|(std::uint32_t(U16(p+2))<<16); }
// Requires only bounded header bytes, for both the file and mapped-image header.
inline bool ParsePe(const std::uint8_t* data,std::size_t bytes,Pe& out) {
    if(!data || bytes<64 || U16(data)!=0x5a4d) return false;
    const std::size_t offset=U32(data+0x3c);
    if(offset<64 || offset>bytes || bytes-offset<24 || U32(data+offset)!=0x4550) return false;
    const auto* nt=data+offset;
    const std::size_t optional=U16(nt+20);
    const auto count=U16(nt+6);
    if(optional<96 || optional>bytes-offset-24 || !count || count>96 || U16(nt+4)!=0x14c || U16(nt+24)!=0x10b) return false;
    Pe p{U32(nt+8),U32(nt+24+56),U32(nt+24+28),U32(nt+24+60),U16(nt+4),U16(nt+24),count};
    if(p.imageBase!=0x10000000 || p.imageSize<p.headersSize || p.headersSize<offset+24+optional || p.imageSize>0x10000000) return false;
    out=p; return true;
}
enum class Build { Unknown=0,Steam=1,Gog=2 };
struct Identity { Digest sha{}; std::uint64_t fileSize=0; Pe pe{}; bool measured=false; };
inline Build Select(const Identity& id) {
    if(!id.measured || id.fileSize!=20183176 || id.pe.machine!=0x14c || id.pe.magic!=0x10b
        || id.pe.timestamp!=0x4aae9636 || id.pe.imageSize!=0x019e8000 || id.pe.imageBase!=0x10000000) return Build::Unknown;
    Digest expected{};
    ParseDigest("6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765",expected);
    return id.sha==expected?Build::Steam:Build::Unknown;
}
// No inferred GOG profile: its table is retained, but an exact full hash/size and
// table provenance must be checked before that profile can be enabled.
inline std::uint32_t Translate(Build build,std::uint32_t va,const std::uint32_t (*map)[2],std::size_t count) {
    if(va<0x10000000 || va>=0x119e8000) return 0;
    if(build==Build::Steam) return va;
    if(build!=Build::Gog || !map) return 0;
    std::size_t low=0,high=count;
    while(low<high) { const auto mid=low+(high-low)/2; if(map[mid][0]<va) low=mid+1; else high=mid; }
    return low<count && map[low][0]==va?map[low][1]:0;
}
} // namespace fc2vr::image
