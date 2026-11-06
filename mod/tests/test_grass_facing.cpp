#include "common/grass_facing.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int checks=0;
static void Check(bool ok,const char* msg){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",msg);std::exit(1);}}
int main(){
    constexpr float pi=3.14159265358979323846f;
    // Manual native yaw remains; only the physical head contribution leaves.
    for(int manual=-180;manual<=180;manual+=15)for(int head=-720;head<=720;head+=5){
        const float native=(manual+head)*pi/180,h=head*pi/180,target=manual*pi/180;
        float m[16]={std::cos(native),-std::sin(native),0,0,std::sin(native),std::cos(native),0,0,0,0,1.4f,0,0,0,0,1};
        float before[16];std::memcpy(before,m,sizeof m);
        Check(fc2vr::grass::RemoveHeadYaw(m,h),"valid cylindrical matrix");
        Check(std::fabs(m[0]-std::cos(target))<0.00001f && std::fabs(m[4]-std::sin(target))<0.00001f,"physical yaw removed, manual yaw retained");
        for(int i=0;i<16;++i)if(i!=0 && i!=1 && i!=4 && i!=5)Check(std::memcmp(m+i,before+i,sizeof(float))==0,"non-facing elements unchanged");
    }
    // A provider uploaded from an older camera must converge to the same world
    // basis, even when its yaw lags a continuously moving head by many degrees.
    for(int head=-180;head<=180;++head)for(int lag=0;lag<=30;lag+=5){
        const float old=(head-lag)*pi/180;
        float m[16]={std::cos(old),-std::sin(old),0,0,std::sin(old),std::cos(old),0,0,0,0,1.4f,0,0,0,0,1};
        Check(fc2vr::grass::SetWorldYaw(m,0.3f),"lagged provider accepts world target");
        Check(std::fabs(m[0]-std::cos(0.3f))<0.000001f && std::fabs(m[4]-std::sin(0.3f))<0.000001f,"world target is independent of provider age");
        float once[16];std::memcpy(once,m,sizeof m);
        Check(fc2vr::grass::SetWorldYaw(m,0.3f) && std::memcmp(once,m,sizeof m)==0,"repeated application cannot accumulate a rotation");
    }
    for(int kind=0;kind<6;++kind){
        float m[16]={1,0,0,0,0,1,0,0,0,0,1.4f,0,0,0,0,1};
        if(kind==0)m[0]=NAN;
        if(kind==1)m[12]=1;
        if(kind==2)m[2]=0.5f;
        if(kind==3)m[0]=2;
        if(kind==4)m[4]=0.1f;
        const float yaw=kind==5?NAN:1;
        float before[16];std::memcpy(before,m,sizeof m);
        Check(!fc2vr::grass::RemoveHeadYaw(m,yaw),"invalid input refused");
        Check(std::memcmp(m,before,sizeof m)==0,"refusal cannot modify input");
    }
    std::printf("grass facing PASS (%d checks)\n",checks);
}
