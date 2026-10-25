#include "common/image_identity.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <initializer_list>
using namespace fc2vr::image;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
std::string Hash(const void* p,std::size_t n,std::size_t chunk=65536) {
    Sha256 s; const auto* bytes=static_cast<const std::uint8_t*>(p);
    while(n) { const auto take=n<chunk?n:chunk; if(!s.Update(bytes,take)) return "error"; bytes+=take; n-=take; }
    Digest d{}; if(!s.Finish(d)) return "error"; char hex[65]; Hex(d,hex); return hex;
}
void Sha() {
    Check(Hash(nullptr,0)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
    Check(Hash("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
    const std::string longVector="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Check(Hash(longVector.data(),longVector.size())=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SHA256 56-byte padding control");
    const std::string million(1000000,'a');
    for(std::size_t chunk:{1u,7u,63u,64u,65u,511u,65536u}) Check(Hash(million.data(),million.size(),chunk)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","million-a streaming control");
    std::vector<std::uint8_t> bytes(513); for(std::size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<std::uint8_t>(i);
    const auto reference=Hash(bytes.data(),bytes.size());
    for(std::size_t size=0;size<=513;++size) {
        const auto value=Hash(bytes.data(),size);
        Check(value==Hash(bytes.data(),size,1),"all padding boundaries agree with bytewise feed");
        Check(size==513 || value!=reference,"different prefix does not collide with full control");
    }
    Sha256 s; Digest first{},again{};
    Check(s.Update("abc",3) && s.Finish(first) && s.Finish(again) && first==again,"repeat finalization stable");
    Check(!s.Update("a",1) && !s.Finish(again),"no append after finalized measurement");
    Sha256 invalid; Check(!invalid.Update(nullptr,1) && !invalid.Finish(again),"null nonempty feed fails");
    Check(!ParseDigest("123",again),"short digest rejected");
}
void Set16(std::vector<std::uint8_t>& b,int at,std::uint16_t v) { b.at(static_cast<std::size_t>(at))=static_cast<std::uint8_t>(v); b.at(static_cast<std::size_t>(at+1))=static_cast<std::uint8_t>(v>>8); }
void Set32(std::vector<std::uint8_t>& b,int at,std::uint32_t v) { Set16(b,at,static_cast<std::uint16_t>(v)); Set16(b,at+2,static_cast<std::uint16_t>(v>>16)); }
std::vector<std::uint8_t> Header() {
    std::vector<std::uint8_t> b(1024); Set16(b,0,0x5a4d); Set32(b,0x3c,128); Set32(b,128,0x4550); Set16(b,132,0x14c); Set16(b,134,7);
    Set32(b,136,0x4aae9636); Set16(b,148,224); Set16(b,152,0x10b); Set32(b,180,0x10000000); Set32(b,208,0x019e8000); Set32(b,212,1024); return b;
}
void Headers() {
    const auto b=Header(); Pe p;
    Check(ParsePe(b.data(),b.size(),p),"bounded x86 PE control");
    for(std::size_t size=0;size<376;++size) Check(!ParsePe(b.data(),size,p),"every truncated NT/optional header rejected");
    for(int cause=0;cause<12;++cause) {
        auto bad=b;
        if(cause==0) bad[0]=0;
        if(cause==1) Set32(bad,0x3c,0xfffffff0);
        if(cause==2) Set32(bad,0x3c,0);
        if(cause==3) bad[128]=0;
        if(cause==4) Set16(bad,132,0x8664);
        if(cause==5) Set16(bad,152,0x20b);
        if(cause==6) Set16(bad,148,65535);
        if(cause==7) Set16(bad,148,0);
        if(cause==8) Set16(bad,134,0);
        if(cause==9) Set16(bad,134,97);
        if(cause==10) Set32(bad,180,0);
        if(cause==11) Set32(bad,212,1);
        Check(!ParsePe(bad.data(),bad.size(),p),"bad architecture/header/range rejected");
    }
    Check(!ParsePe(nullptr,1024,p),"null header unavailable");
}
Identity Known() {
    Identity id; id.measured=true; id.fileSize=20183176;
    ParseDigest("6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765",id.sha);
    const auto header=Header(); ParsePe(header.data(),header.size(),id.pe); return id;
}
void Profiles() {
    const auto known=Known(); Check(Select(known)==Build::Steam,"exact Steam profile");
    for(int i=0;i<32;++i) { auto other=known; other.sha[i]^=1; Check(Select(other)==Build::Unknown,"every hash byte is authoritative"); }
    for(int cause=0;cause<8;++cause) {
        auto other=known;
        if(cause==0) other.measured=false;
        if(cause==1) --other.fileSize;
        if(cause==2) other.pe.machine=0x8664;
        if(cause==3) other.pe.magic=0x20b;
        if(cause==4) ++other.pe.timestamp;
        if(cause==5) ++other.pe.imageSize;
        if(cause==6) ++other.pe.imageBase;
        if(cause==7) other.sha={};
        Check(Select(other)==Build::Unknown,"unknown or incomplete identity never gets a profile");
    }
    auto modernized=known; modernized.fileSize=20178432; modernized.sha[0]^=1;
    Check(Select(modernized)==Build::Unknown,"same timestamp, distinct Modernized image refused");
    auto gog=known; gog.pe.timestamp=0x49fb4bf6; gog.pe.imageSize=0x0192a000;
    Check(Select(gog)==Build::Unknown,"unverified GOG profile refused rather than guessed");
}
void Translation() {
    const std::uint32_t map[][2]={{0x10001000,0x10002000},{0x10002000,0x10003000}};
    for(std::uint32_t va:{0u,0x10001000u,0x1040c800u,0x11800000u,0xffffffffu}) Check(Translate(Build::Unknown,va,map,2)==0,"unknown cannot authorize any address");
    Check(Translate(Build::Steam,0x1040c800,map,2)==0x1040c800,"accepted Steam address unchanged");
    Check(Translate(Build::Steam,0x119e8000,map,2)==0 && Translate(Build::Steam,0xffffffff,map,2)==0,"address outside supported module rejected");
    Check(Translate(Build::Gog,0x10001000,map,2)==0x10002000,"explicit mapped profile contract retained");
    Check(Translate(Build::Gog,0x10003000,map,2)==0,"missing GOG entry refused");
    Check(Translate(Build::Gog,0x10001000,nullptr,2)==0,"missing translation table refused");
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!std::strcmp(argv[1],"sha")) Sha();
    else if(!std::strcmp(argv[1],"headers")) Headers();
    else if(!std::strcmp(argv[1],"profiles")) Profiles();
    else if(!std::strcmp(argv[1],"translation")) Translation();
    else return 2;
    std::printf("%s: %d checks, %d failures\n",argv[1],checks,failures); return failures?1:0;
}
