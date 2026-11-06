#include "common/shoulder_frame.h"
#include <cstdio>
#include <cstdlib>
static int checks=0;
static void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    constexpr float pi=3.14159265358979323846f;
    const float up[3]={0,0,1};
    for(int degree=-720;degree<=720;++degree){
        const float yaw=degree*pi/180;
        for(int pitch=-80;pitch<=80;pitch+=20){
            const float p=pitch*pi/180;
            float forward[3]={std::cos(yaw)*std::cos(p),std::sin(yaw)*std::cos(p),std::sin(p)},right[3]{};
            Check(fc2vr::shoulder::Basis(forward,up,right),"valid shoulder frame");
            const float expected[3]={std::sin(yaw),-std::cos(yaw),0};
            float side=0;for(int i=0;i<3;++i)side+=right[i]*expected[i];
            Check(side>0.9999f,"right shoulder stays on head's right through large turns and wrap");
            const float rightShoulder[3]={right[0]*0.2f,right[1]*0.2f,-0.2f};
            const float leftShoulder[3]={-right[0]*0.2f,-right[1]*0.2f,-0.2f};
            float separation=0;for(int i=0;i<3;++i)separation+=(rightShoulder[i]-leftShoulder[i])*expected[i];
            Check(separation>0.3999f,"shoulder origins cannot exchange sides");
        }
    }
    float vertical[3]={0,0,1},right[3]{};
    Check(!fc2vr::shoulder::Basis(vertical,up,right),"vertical projection requests body fallback");
    float invalid[3]={NAN,0,0};Check(!fc2vr::shoulder::Basis(invalid,up,right),"nonfinite projection requests fallback");
    std::printf("shoulder frame PASS (%d checks)\n",checks);
}
