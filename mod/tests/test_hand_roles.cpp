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
struct ItemFrame {
    bool handValid=true,leftValid=false,handAuthority=true,leftAuthority=false,twoHand=true;
    float rSqueeze=.8f,lSqueeze=.2f;
    float aimO[3]{},aimR[3]{},aimU[3]{},aimB[3]{},gripO[3]{};
    float laimO[3]{},laimR[3]{},laimU[3]{},laimB[3]{},lgripO[3]{};
    unsigned source=123;
};
int main() {
    Hand physical[2]{};
    physical[0]={{0.25f,0.75f},true,true,true,false,0.8f,0.2f,-0.3f,true,true};
    physical[1]={{-0.6f,-0.1f},false,false,false,true,0.1f,0.9f,0.4f,true,true};
    for(bool left:{false,true}) {
        fc2vr::hands::Configure(left);
        Hand roles[2];fc2vr::hands::Controls(physical,roles);
        const int weapon=left?0:1,support=1-weapon;
        for(bool held:{false,true})for(bool map:{false,true})for(bool normal:{false,true}) {
            Check(fc2vr::hands::WeaponRig(held,map,normal,true)==(left && held && !map && normal),"weapon vs anatomical item layout");
            Check(!fc2vr::hands::WeaponRig(held,map,normal,false),"missing native left bone cannot select weapon rig");
        }
        Check(fc2vr::hands::Weapon()==weapon && fc2vr::hands::Support()==support,"physical roles");
        Check(fc2vr::hands::Physical(-1)==-1,"whole-source freshness sentinel");
        Check(roles[1].trigger==physical[weapon].trigger,"fire follows weapon trigger");
        Check(roles[0].trigger==physical[support].trigger,"reload follows support trigger");
        Check(roles[1].position==physical[weapon].position,"weapon pose follows physical weapon hand");
        Check(roles[0].squeeze==physical[support].squeeze,"foregrip follows support hand");
        Check(roles[1].btn1==physical[1].btn1 && roles[0].btn2==physical[0].btn2,"face buttons stay physical");
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
    ItemFrame item;
    for(int i=0;i<3;++i){
        item.aimO[i]=float(10+i);item.laimO[i]=float(20+i);
        item.aimR[i]=float(30+i);item.laimR[i]=float(40+i);
        item.aimU[i]=float(50+i);item.laimU[i]=float(60+i);
        item.aimB[i]=float(70+i);item.laimB[i]=float(80+i);
        item.gripO[i]=float(90+i);item.lgripO[i]=float(100+i);
    }
    fc2vr::hands::AnatomicalItems(item);
    Check(!item.handValid && item.leftValid && !item.handAuthority && item.leftAuthority,"item pose validity/authority follow the physical hand, including dropout");
    Check(!item.twoHand && item.rSqueeze==.2f && item.lSqueeze==.8f,"utility mode cannot inherit weapon foregrip latch");
    Check(item.source==123,"anatomical conversion never remaps physical source stamp");
    for(int i=0;i<3;++i){
        Check(item.aimO[i]==20+i && item.laimO[i]==10+i,"map/radio origin remains on anatomical hand");
        Check(item.aimR[i]==40+i && item.laimR[i]==30+i,"item right basis");
        Check(item.aimU[i]==60+i && item.laimU[i]==50+i,"item up basis");
        Check(item.aimB[i]==80+i && item.laimB[i]==70+i,"item back basis");
        Check(item.gripO[i]==100+i && item.lgripO[i]==90+i,"item grip positions");
    }
    fc2vr::hands::Configure(false);
    std::puts("PASS: weapon/support controls, physical sticks and per-hand authority in both modes");
}
