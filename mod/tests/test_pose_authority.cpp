#include "common/pose_authority.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
using namespace fc2vr::authority;
namespace {
int checks = 0, failures = 0;
void Check(bool ok,const char* why) { ++checks; if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",why); } }
Stamp Sample() {
    Stamp s; s.sequence = 20; s.receivedMs = 1000; s.generation = 3;
    s.handGeneration[0] = 4; s.handGeneration[1] = 5; s.focused = s.synced = true; return s;
}
void Freshness() {
    const auto good = Sample();
    for (std::uint64_t age = 0; age <= 100; ++age) Check(Fresh(good,good,1000+age),"fresh sample accepted including boundary");
    Check(!Fresh(good,good,1101),"expired at 101ms");
    Check(!Fresh(good,good,999),"backwards/future receipt rejected");
    for (int cause = 0; cause < 7; ++cause) {
        auto current = good;
        if (cause == 0) current.focused = false;
        if (cause == 1) current.synced = false;
        if (cause == 2) ++current.generation;
        if (cause == 3) current.receivedMs = 1200;
        if (cause == 4) current.receivedMs = 0;
        if (cause == 5) current.sequence = 0;
        if (cause == 6) --current.sequence;
        Check(!Fresh(good,current,1050),"current state independently invalidates an old source");
    }
    auto leftLoss = good; ++leftLoss.handGeneration[0];
    Check(!Fresh(good,leftLoss,1001,0),"left tracking epoch invalidates left lease");
    Check(Fresh(good,leftLoss,1001,1),"independent right lease survives left loss");
    auto headLoss = good; ++headLoss.handGeneration[0]; ++headLoss.handGeneration[1];
    Check(Fresh(good,headLoss,1001),"head loss does not revoke pose-independent buttons");
    Check(!Fresh(good,headLoss,1001,0) && !Fresh(good,headLoss,1001,1),"head loss invalidates both physical hand cohorts");
    Check(!Fresh(Stamp{},good,1001),"unpublished source rejected");
    auto inferred = good; inferred.focused = false;
    Check(!Fresh(inferred,good,1001),"unfocused source cannot become authoritative later");
    inferred = good; inferred.predictedTime = std::numeric_limits<std::int64_t>::max();
    Check(!Fresh(inferred,good,1101),"future XR prediction cannot extend receipt age");
    // Negative control: the old ever-published check accepts an expired sample.
    Check(good.sequence != 0 && !Fresh(good,good,1200),"old no-age route disagrees on expiry");
}
struct Q { float x=0,y=0,z=0,w=1; };
struct P { float x=0,y=0,z=0; };
struct Pose { Q orientation{}; P position{}; };
void Cache() {
    PoseCache<Pose> cache;
    const PoseEvidence tracked{true,true,true,true,true};
    Pose raw; raw.position = {1,2,3};
    Check(!cache.Resolve(raw,{},true).visualValid,"empty cache cannot invent a pose");
    auto visual = cache.Resolve(raw,tracked,true);
    Check(visual.visualValid && !visual.held && tracked.Tracked(),"tracked live pose");
    PoseEvidence lostPosition{true,false,true,false,true};
    raw.position = {99,99,99}; raw.orientation = {0,0.7071068f,0,0.7071068f};
    visual = cache.Resolve(raw,lostPosition,true);
    Check(visual.visualValid && visual.held,"held position remains visible");
    Check(visual.pose.position.x == 1 && visual.pose.orientation.y == raw.orientation.y,"held position plus live orientation");
    Check(!lostPosition.Valid() && !lostPosition.Tracked(),"fallback never promotes raw evidence");
    const PoseEvidence inactive{};
    visual = cache.Resolve({},inactive,true);
    Check(visual.visualValid && visual.held && !inactive.Tracked(),"inactive cached pose is cosmetic only");
    Check(!cache.Resolve({},inactive,false).visualValid,"HoldLostTracking off disables visual fallback");
    const PoseEvidence validUntracked{true,true,true,false,false};
    Check(cache.Resolve(raw,validUntracked,true).visualValid && !validUntracked.Tracked(),"valid estimated pose is not tracked authority");
    cache.Clear();
    Check(!cache.Resolve({},inactive,true).visualValid,"space/session cache reset discards old coordinates");
    // Negative control: legacy valid=true would authorize the held pose.
    cache.Resolve(raw,tracked,true);
    visual = cache.Resolve({},inactive,true);
    Check(visual.visualValid && !inactive.Tracked(),"legacy visual-valid permission is rejected");
    for (int flags = 0; flags < 32; ++flags) {
        const PoseEvidence e{(flags&1)!=0,(flags&2)!=0,(flags&4)!=0,(flags&8)!=0,(flags&16)!=0};
        Check(e.Tracked() == (flags == 31),"all active/valid/tracked components are required");
        const auto cached = cache.Resolve(raw,e,true);
        Check(cached.visualValid,"visual cache can survive every partial flag combination");
        Check(e.Tracked() == (flags == 31),"resolving display cache cannot change evidence");
    }
}
void Rearm() {
    RearmGate buttons,poses;
    Check(!buttons.Update(true,false),"held button on startup is blocked");
    Check(buttons.Update(true,true),"neutral fresh sample arms buttons");
    Check(poses.Update(true,true),"tracked neutral sample arms physical poses");
    Check(buttons.Update(true,false),"fresh press accepted");
    Check(!poses.Update(false,false),"tracking loss revokes physical poses");
    Check(buttons.Update(true,false),"ordinary buttons survive position loss");
    for (int i = 0; i < 100; ++i) Check(!poses.Update(true,false),"tracking recovery while gripping cannot reengage");
    Check(poses.Update(true,true) && poses.Update(true,false),"release then fresh grip reengages");
    Check(!buttons.Update(false,false),"focus/sync/age loss revokes buttons");
    for (int i = 0; i < 100; ++i) Check(!buttons.Update(true,false),"held-through-focus buttons stay blocked");
    Check(buttons.Update(true,true) && buttons.Update(true,false),"neutral then fresh press restores buttons");
    buttons.Clear(); poses.Clear();
    Check(!buttons.Update(true,false) && !poses.Update(true,false),"recenter/space change requires neutral again");
}
void Cursor() {
    SampleCursor cursor; auto sample = Sample(); int mapped = 0;
    for (int poll = 0; poll < 100; ++poll) if (cursor.Take(sample)) ++mapped;
    Check(mapped == 1,"100 XInput polls advance mapping once");
    --sample.sequence; Check(!cursor.Take(sample),"old sequence cannot replay gestures");
    sample.sequence += 2; Check(cursor.Take(sample),"next sample advances mapping");
    ++sample.generation; sample.sequence = 1; Check(cursor.Take(sample),"new generation can restart sequence");
    Check(!cursor.Take(Stamp{}),"no publication is not an input sample");
    cursor.Clear(); Check(cursor.Take(sample),"explicit reset clears cursor");
}
void Safety() {
    const auto original = Sample(); auto live = original;
    int weaponA = 0, weaponB = 0;
    Check(SameOwner(&weaponA,&weaponA),"same current weapon");
    Check(!SameOwner(&weaponA,&weaponB) && !SameOwner(nullptr,nullptr),"swap and no owner rejected");
    for (int repeat = 0; repeat < 20; ++repeat) {
        live.sequence++; live.receivedMs += 16;
        const bool authorized = SameOwner(&weaponA,&weaponA) && Fresh(original,live,live.receivedMs,1);
        Check(authorized == (live.receivedMs <= 1100),"redrawing/reapplying an old pose cannot renew source age");
    }
    live = original; ++live.handGeneration[1];
    Check(!Fresh(original,live,1001,1),"regained hand tracking cannot revive pre-loss shot cache");
    live = original; ++live.generation;
    Check(!Fresh(original,live,1001),"reference/session/focus generation invalidates all consumers");
    auto fresh = live; ++fresh.sequence; fresh.receivedMs = 1001;
    Check(Fresh(fresh,live,1001) == false,"unpublished future sequence is refused");
    live = original; ++live.sequence; live.receivedMs = 1500;
    Check(!Fresh(original,live,1500,1),"old raw pose cannot control a delayed action");
    Check(SameContext(original,live,1) && Fresh(live,live,1500),"accepted launch snapshot survives normal animation delay in a live context");
    ++live.generation;
    Check(!SameContext(original,live,1),"focus/session/reference change cancels accepted launch context");
}
void Finite() {
    for (float bad : {std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        Check(FiniteAxis(bad) == 0,"nonfinite input cannot become stick/trigger output");
    Check(FiniteAxis(2) == 1 && FiniteAxis(-2) == -1,"signed stick bounds");
    Check(FiniteAxis(-1,0,1) == 0 && FiniteAxis(0.5f,0,1) == 0.5f,"unsigned trigger bounds");
    Pose p;
    Check(FinitePosition(p) && FiniteOrientation(p),"finite identity pose");
    p.position.x = std::numeric_limits<float>::quiet_NaN();
    Check(!FinitePosition(p),"invalid view position cannot poison recenter/height reference");
    p.orientation.w = 0;
    Check(!FiniteOrientation(p),"zero quaternion rejected");
    p.orientation.x = std::numeric_limits<float>::infinity();
    Check(!FiniteOrientation(p),"nonfinite quaternion rejected");
}
} // namespace
int main(int argc,char** argv) {
    if (argc != 2) return 2;
    const char* group = argv[1];
    if (!std::strcmp(group,"freshness")) Freshness();
    else if (!std::strcmp(group,"cache")) Cache();
    else if (!std::strcmp(group,"rearm")) Rearm();
    else if (!std::strcmp(group,"cursor")) Cursor();
    else if (!std::strcmp(group,"safety")) Safety();
    else if (!std::strcmp(group,"finite")) Finite();
    else return 2;
    std::printf("%s: %d checks, %d failures\n",group,checks,failures); return failures ? 1 : 0;
}
