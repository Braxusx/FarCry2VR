#include "common/hand_roles.h"
#include "common/pose_authority.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void Check(bool value,const char* label) {
    if(!value){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}
}
struct Hand {
    struct Stick { float x=0,y=0; } stick;
    bool stickClick=false,menu=false,btn1=false,btn2=false;
    float trigger=0,squeeze=0,position=0;
    bool gripValid=false,aimValid=false;
};
int main() {
    Hand physical[2]{};
    physical[0]={{0.25f,0.75f},true,true,true,false,0.8f,0.2f,-0.3f,true,true};
    physical[1]={{-0.6f,-0.1f},false,false,false,true,0.1f,0.9f,0.4f,true,true};
    for(bool left:{false,true}) {
        fc2vr::hands::Configure(left);
        Hand roles[2];fc2vr::hands::Controls(physical,roles);
        const int weapon=left?0:1,support=1-weapon;
        Check(fc2vr::hands::Weapon()==weapon && fc2vr::hands::Support()==support,"physical roles");
        Check(fc2vr::hands::Physical(-1)==-1,"whole-source freshness sentinel");
        Check(roles[1].trigger==physical[weapon].trigger,"fire follows weapon trigger");
        Check(roles[0].trigger==physical[support].trigger,"reload follows support trigger");
        Check(roles[1].position==physical[weapon].position,"weapon pose follows physical weapon hand");
        Check(roles[0].squeeze==physical[support].squeeze,"foregrip follows support hand");
        Check(roles[1].btn1==physical[weapon].btn1 && roles[0].btn2==physical[support].btn2,"face buttons follow action roles");
        for(int i=0;i<2;++i) {
            Check(roles[i].stick.x==physical[i].stick.x && roles[i].stick.y==physical[i].stick.y,"sticks stay physical");
            Check(roles[i].stickClick==physical[i].stickClick,"sprint/heal stick clicks stay physical");
            Check(roles[i].menu==physical[i].menu,"pause stays on physical menu button");
        }
        using fc2vr::authority::Stamp;
        Stamp source{};source.sequence=1;source.receivedMs=100;source.generation=3;
        source.handGeneration[0]=11;source.handGeneration[1]=23;source.focused=source.synced=true;
        auto current=source;
        ++current.handGeneration[support];
        Check(fc2vr::authority::Fresh(source,current,110,fc2vr::hands::Weapon()),"support dropout does not substitute weapon authority");
        Check(!fc2vr::authority::Fresh(source,current,110,fc2vr::hands::Support()),"support dropout cancels support authority");
        current=source;++current.handGeneration[weapon];
        Check(!fc2vr::authority::Fresh(source,current,110,fc2vr::hands::Weapon()),"weapon dropout cancels weapon authority even if other hand tracks");
        Check(!fc2vr::authority::Fresh(source,source,201,fc2vr::hands::Weapon()),"stale weapon source rejected");
        physical[weapon].aimValid=false;
        fc2vr::hands::Controls(physical,roles);
        Check(!roles[1].aimValid && roles[0].aimValid,"failed pose stays with its physical hand");
        physical[weapon].aimValid=true;
    }
    fc2vr::hands::Configure(false);
    std::puts("PASS: weapon/support controls, physical sticks and per-hand authority in both modes");
}
