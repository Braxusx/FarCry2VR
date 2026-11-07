#include "common/hand_roles.h"
#include "common/hand_switch.h"
#include "common/pose_authority.h"
#include "common/melee_motion.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void Check(bool ok,const char* label){if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}}
int main(){
    using namespace fc2vr::hands;
    ToggleKey key;
    key.Poll(true,true);Check(key.pending,"first F1 press queues switch");key.Complete();
    for(int i=0;i<100;++i){key.Poll(true,true);Check(!key.pending,"held F1 does not repeat");}
    key.Poll(false,true);key.Poll(true,true);Check(key.pending,"fresh press queues another switch");
    key.Poll(false,true);key.Poll(true,true);Check(!key.pending,"second press cancels a busy queued request");
    key.Poll(false,true);key.Poll(true,false);key.Poll(true,true);Check(!key.pending,"focus return / modifier release with held F1 cannot create an edge");
    key.Poll(false,true);key.Poll(true,true);key.Poll(false,false);Check(!key.pending,"focus loss cancels pending switch");
    {
        ReadGuard outer;ReadGuard nested;
        TryWriteGuard denied;Check(!denied,"nested reader cannot upgrade or deadlock");
    }
    {TryWriteGuard writer;Check(bool(writer),"writer acquired after nested readers exit");}
    std::atomic<bool> ready{false},finish{false};
    std::thread reader([&]{ReadGuard guard;ready=true;while(!finish.load())std::this_thread::yield();});
    while(!ready.load())std::this_thread::yield();
    {TryWriteGuard writer;Check(!writer,"render transition defers while a game reader is active");}
    finish=true;reader.join();
    fc2vr::authority::Stamp old{};old.sequence=1;old.receivedMs=100;old.generation=1;old.focused=old.synced=true;
    auto current=old;
    Check(fc2vr::melee_motion::Continuous(1,1,1,1,old,current,0,.016),"normal blade samples retain motion");
    Check(!fc2vr::melee_motion::Continuous(1,1,1,2,old,current,0,.016),"F1 hand displacement cannot synthesize a machete swing");
    Check(!fc2vr::melee_motion::Continuous(1,2,1,1,old,current,0,.016),"drawing another weapon cannot synthesize a swing");
    Check(!fc2vr::melee_motion::Continuous(1,1,1,1,old,current,0,.3),"long observation gap rebaselines blade motion");
    ++current.handGeneration[0];
    Check(!fc2vr::melee_motion::Continuous(1,1,1,1,old,current,0,.016),"weapon tracking rearm rebaselines blade motion");
    Check(fc2vr::melee_motion::Continuous(1,1,1,1,old,current,1,.016),"support dropout does not borrow or cancel weapon motion context");
    current=old;
    for(int i=0;i<4;++i){
        const bool wasLeft=leftHanded;const auto oldLayout=Generation();
        TryWriteGuard writer;Check(bool(writer),"transition exclusive lock available");
        // The native adapter invalidates the XR lease before changing layout.
        ++current.generation;current.synced=false;
        Configure(!wasLeft);
        Check(Generation()==oldLayout+1 && bool(leftHanded)!=wasLeft,"layout generation advances on each actual switch");
        for(int physical=0;physical<2;++physical){
            Check(!fc2vr::authority::Fresh(old,current,110,physical),"old input cannot acquire the new physical role");
            fc2vr::authority::RearmGate gate;
            Check(!gate.Update(true,false),"held trigger/grip stays disarmed after switch");
            Check(gate.Update(true,true),"release rearms hand");
            Check(gate.Update(true,false),"next intentional press is accepted");
        }
        current.synced=true;++current.sequence;current.receivedMs=110;
        for(int physical=0;physical<2;++physical){
            Check(!fc2vr::authority::Fresh(old,current,115,physical),"old hand lease stays invalid after new tracking/input resumes");
            Check(fc2vr::authority::Fresh(current,current,115,physical),"new physical input lease can become current normally");
        }
        Configure(bool(leftHanded));Check(Generation()==oldLayout+1,"unchanged layout does not advance generation");
    }
    std::atomic<bool> bad{false};finish=false;
    std::thread racingReader([&]{while(!finish.load()){
        ReadGuard guard;const bool left=leftHanded;const auto generation=Generation();std::this_thread::yield();
        if(left!=bool(leftHanded) || generation!=Generation())bad=true;
    }});
    for(int i=0;i<1000;++i){TryWriteGuard writer;if(writer)Configure(!leftHanded);std::this_thread::yield();}
    finish=true;racingReader.join();Check(!bad,"readers retain one layout through concurrent switch attempts");
    Configure(false);
    std::puts("PASS: F1 edges/focus/cancel, nested/concurrent layout readers, lease rejection and neutral rearm");
}
