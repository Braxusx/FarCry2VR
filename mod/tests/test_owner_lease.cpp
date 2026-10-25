#include "common/owner_lease.h"
#include "common/pose_authority.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace fc2vr::owner;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
Key Player() { Key k; k.actor=1;k.data=2;k.graphics=3;k.rig=4;k.palette=5;k.count=130;k.left=20;k.right=40;return k; }
Key Weapon() { auto k=Player();k.weapon=6;k.handle=7;k.entity=8;k.modelGraphics=9;k.model=10;k.modelPalette=11;k.attachment=12;k.modelCount=8;return k; }
void Keys() {
    Check(!Key{}.Valid() && Player().Valid() && Weapon().Valid(),"invalid, empty hands and full weapon controls");
    for(auto field:{&Key::actor,&Key::data,&Key::graphics,&Key::rig,&Key::palette,&Key::weapon,&Key::handle,&Key::entity,&Key::modelGraphics,&Key::model,&Key::modelPalette,&Key::attachment}) {
        auto k=Weapon();k.*field=0;Check(!k.Valid(),"every required owner field validated");
        k=Weapon();++(k.*field);Check(k.Valid() && !(k==Weapon()),"every observed identity participates in equality");
    }
    for(auto field:{&Key::count,&Key::left,&Key::right,&Key::modelCount}) { auto k=Weapon();++(k.*field);Check(k.Valid() && !(k==Weapon()),"count and helper slots participate in equality"); }
    for(int count:{-1,0,513}) { auto k=Weapon();k.count=count;Check(!k.Valid(),"invalid player palette count");k=Weapon();k.modelCount=count;Check(!k.Valid(),"invalid weapon palette count"); }
    auto k=Player();k.left=k.right;Check(!k.Valid(),"palm indices must differ");k=Player();k.left=-1;Check(!k.Valid(),"missing palm rejected");k=Player();k.right=k.count;Check(!k.Valid(),"out of range palm rejected");
    for(auto field:{&Key::handle,&Key::entity,&Key::modelGraphics,&Key::model,&Key::modelPalette,&Key::attachment}) { k=Player();k.*field=1;Check(!k.Valid(),"partial weapon tuple never becomes empty hands"); }
    k=Player();k.modelCount=1;Check(!k.Valid(),"orphan model count rejected");
}
void Epochs() {
    Tracker t;Check(!t.Read().valid && !Matches(t.Read(),t.Read()),"unobserved snapshot has no authority");
    Check(t.Observe(Weapon(),1000),"first complete observation publishes");const auto a=t.Read();
    Check(!t.Observe(Weapon(),1010) && Matches(a,t.Read()),"continuous same cohort retains epoch");
    for(auto field:{&Key::actor,&Key::data,&Key::graphics,&Key::rig,&Key::palette,&Key::handle,&Key::entity,&Key::modelGraphics,&Key::model,&Key::modelPalette,&Key::attachment}) {
        auto changed=Weapon();++(changed.*field);const auto previous=t.Read();
        Check(t.Observe(changed,1020) && !Matches(previous,t.Read()),"same weapon pointer cannot hide changed native cohort");
        Check(t.Observe(Weapon(),1020) && !Matches(a,t.Read()),"A to B to A does not revive old lease");
    }
    for(auto field:{&Key::count,&Key::left,&Key::right,&Key::modelCount}) { auto changed=Weapon();++(changed.*field);const auto previous=t.Read();Check(t.Observe(changed,1030) && !Matches(previous,t.Read()),"metadata changes revoke cohort");t.Observe(Weapon(),1030); }
    const auto before=t.Read();Check(t.Observe({},1040) && !t.Read().valid && !Matches(before,t.Read()),"missing owner revokes");
    const auto invalid=t.Read();Check(!t.Observe({},1041) && t.Read().epoch==invalid.epoch,"repeated missing owner does not churn");
    Check(t.Observe(Weapon(),1050) && !Matches(before,t.Read()),"return after missing owner gets new epoch");
    Check(t.Observe(Player(),1060) && !Matches(before,t.Read()),"holstered to empty hands revokes weapon products");
}
void Heartbeat() {
    Tracker t;t.Observe(Weapon(),1000);const auto first=t.Read();
    Check(Fresh(first,1000) && Fresh(first,1250),"inclusive owner heartbeat window");
    Check(!Fresh(first,999) && !Fresh(first,1251) && !Fresh({},1000),"future, stale and invalid observation rejected");
    Check(t.Observe(Weapon(),1251) && !Matches(first,t.Read()),"same pointer after producer gap cannot revive lease");
    const auto resumed=t.Read();Check(!t.Observe(Weapon(),1252) && Matches(resumed,t.Read()) && t.Read().observedMs==1252,"heartbeat updates without churn");
    Check(t.Observe(Weapon(),1200) && !Matches(resumed,t.Read()),"backward clock starts a new cohort");
    fc2vr::authority::Stamp pose;pose.sequence=1;pose.receivedMs=1300;pose.generation=2;pose.focused=pose.synced=true;
    t.Observe(Weapon(),1300);const auto lease=t.Read();
    Check(Matches(lease,t.Read()) && Fresh(t.Read(),1400) && fc2vr::authority::Fresh(pose,pose,1400,1),"pose and owner positive control");
    t.Observe(Weapon(),1401);
    Check(Fresh(t.Read(),1401) && !fc2vr::authority::Fresh(pose,pose,1401,1),"owner heartbeat never refreshes an old physical pose");
    fc2vr::authority::RearmGate fire;
    Check(!fire.Update(true,false) && fire.Update(true,true) && fire.Update(true,false),"fire needs neutral rearm then follows held trigger");
    fire.Clear();Check(!fire.Update(true,false),"owner epoch change cancels held-through fire");
    Check(!fire.Update(false,true) && !fire.Update(true,false) && fire.Update(true,true),"missing owner cannot arm fire");
}
void Coverage() {
    int map[512]{};
    for(int bones=1;bones<=64;++bones) {
        for(int i=0;i<bones;++i) map[i]=bones-1-i;
        Check(CoversPalette(bones,map,bones),"complete reverse map accepted");
        Check(!CoversPalette(bones-1,map,bones),"partial update rejected");
        for(int i=bones;i<512;++i) map[i]=-1;
        Check(CoversPalette(512,map,bones),"skips and complete coverage accepted");
        map[511]=bones;Check(!CoversPalette(512,map,bones),"out of range map rejected");
        map[511]=-2;Check(!CoversPalette(512,map,bones),"invalid negative map rejected");
        map[511]=-1;map[0]=bones>1?map[1]:-1;Check(!CoversPalette(512,map,bones),"duplicates cannot stand in for a missing bone");
    }
    Check(!CoversPalette(1,nullptr,1) && !CoversPalette(513,map,1) && !CoversPalette(1,map,65) && !CoversPalette(1,map,0),"map bounds validated before indexing");
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!std::strcmp(argv[1],"keys")) Keys();else if(!std::strcmp(argv[1],"epochs")) Epochs();else if(!std::strcmp(argv[1],"heartbeat")) Heartbeat();else if(!std::strcmp(argv[1],"coverage")) Coverage();else return 2;
    std::printf("%s: %d checks, %d failures\n",argv[1],checks,failures);return failures?1:0;
}
