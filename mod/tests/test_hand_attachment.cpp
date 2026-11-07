#include "common/hand_attachment.h"
#include "common/left_attachment_policy.h"
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Matrix { float m[16]{}; };
static Matrix Pose(float yaw,float pitch,float x,float y,float z) {
    const float c=std::cos(yaw),s=std::sin(yaw),a=std::cos(pitch),b=std::sin(pitch);
    return {{c,s,0,0,-s*a,c*a,b,0,s*b,-c*b,a,0,x,y,z,1}};
}
static Matrix Inverse(const Matrix& m) {
    Matrix out{};out.m[15]=1;
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)out.m[i*4+j]=m.m[j*4+i];
    for(int j=0;j<3;++j)for(int k=0;k<3;++k)out.m[12+j]-=m.m[12+k]*out.m[k*4+j];
    return out;
}
static Matrix Mul(const Matrix& a,const Matrix& b) {
    Matrix out{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)out.m[i*4+j]+=a.m[i*4+k]*b.m[k*4+j];return out;
}
static float Error(const Matrix& a,const Matrix& b) {
    float e=0;for(int i=0;i<16;++i)e=std::fmax(e,std::fabs(a.m[i]-b.m[i]));return e;
}
int main() {
    using fc2vr::left_attachment::NativeOffset;using fc2vr::left_attachment::Offset;
    auto check=[](bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}};
    check(NativeOffset(false,true,false,81,81,56,false,false)==Offset::Capture,"first native right mount is captured");
    check(NativeOffset(false,true,true,15,81,56,false,false)==Offset::Reject,"reload cannot borrow an unknown rest offset");
    check(NativeOffset(false,true,false,81,81,56,true,false)==Offset::Reject,"converted offset cannot become an initial rest");
    check(NativeOffset(true,false,false,81,81,56,false,false)==Offset::Capture,"non-frozen native animations can update their offset");
    check(NativeOffset(true,false,false,81,81,56,true,false)==Offset::Reuse,"never capture our converted offset");
    check(NativeOffset(true,true,false,15,81,56,false,false)==Offset::Reject,"non-reload holster offsets remain native");
    check(NativeOffset(true,true,false,15,81,56,false,true)==Offset::Reuse,"existing zero-offset reattachment recovery remains supported");
    const Matrix rest=Pose(.2f,-.1f,.03f,-.02f,.07f);Matrix cached=rest;
    for(int frame=0;frame<600;++frame){
        const int indices[]={81,56,15,105};const int index=indices[frame%4];
        const Matrix animated=Pose(frame*.03f,.6f,.35f+frame*.0001f,.2f,-.1f);
        const auto action=NativeOffset(true,true,true,index,81,56,false,false);
        check(action==Offset::Reuse,"all active-reload parent/offset changes retain the known native rest");
        if(action==Offset::Capture)cached=animated;
        check(Error(cached,rest)==0,"RPG animation cannot drift the cached rest onto the right side");
    }
    check(NativeOffset(true,true,false,81,81,56,false,false)==Offset::Reuse,"animation tail after reload completion cannot poison rest");
    int checks=0;
    for(int turn=-720;turn<=720;turn+=3)for(int pitch=-60;pitch<=60;pitch+=20) {
        const float radians=turn*0.01745329252f,p=pitch*0.01745329252f;
        const Matrix grip=Pose(radians,p,-0.25f,1.3f,-0.4f);
        const Matrix nativeLeft=Pose(radians+0.7f,p-0.2f,-0.25f,1.3f,-0.4f);
        Matrix base=Pose(0.2f,-0.1f,0.03f,-0.02f,0.07f);
        for(int i=0;i<3;++i)base.m[i]*=1.2f;
        const Matrix inverse=Inverse(nativeLeft);
        Matrix converted{},again{};
        fc2vr::hand_attachment::Compose(base.m,grip.m,inverse.m,converted.m);
        fc2vr::hand_attachment::Compose(base.m,grip.m,inverse.m,again.m);
        if(Error(Mul(converted,nativeLeft),Mul(base,grip))>0.00001f || Error(converted,again)>0){
            std::fputs("FAIL: native left attachment changes calibrated gun pose or accumulates\n",stderr);return 1;
        }
        if(Error(Mul(base,nativeLeft),Mul(base,grip))<0.1f){
            std::fputs("FAIL: regression case does not distinguish unchanged right-hand offset\n",stderr);return 1;
        }
        ++checks;
    }
    std::printf("PASS: %d left-hand attachment poses preserve gun alignment and repeat without drift\n",checks);
}
