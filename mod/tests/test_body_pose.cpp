#include "common/body_pose.h"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace fc2vr::tracking;
int main() {
    int failures=0;
    auto check=[&](bool condition,const char* message) { if(!condition) { std::fprintf(stderr,"FAIL: %s\n",message); ++failures; } };
    auto near=[](float a,float b) { return std::fabs(a-b)<0.0001f; };
    float root[16]={1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
    BodyPoseHistory history;
    check(!history.Capture().latest.valid,"uninitialized body is unavailable");
    history.Publish({10,20,31.5f},root,1,2,1000);
    check(!history.Capture().previous.valid,"first update has no invented previous pose");
    root[12]=10.12f;
    history.Publish({10.12f,20,31.5f},root,1,2,1011.11);
    const auto cameraSnapshot=history.Capture();
    const auto cameraAnchor=AnchorFor(cameraSnapshot.Select(true),true);
    check(cameraAnchor.valid && cameraAnchor.root && near(cameraAnchor.position.x,10),"camera uses requested previous seat");
    // Worker publication between camera preparation and hand submission. The
    // former second read selected a different previous update (12 cm ahead).
    root[12]=10.24f;
    history.Publish({10.24f,20,31.5f},root,1,2,1022.22);
    const auto handsAnchor=cameraAnchor;
    const auto legacySecondRead=AnchorFor(history.Capture().Select(true),true);
    check(near(handsAnchor.position.x,cameraAnchor.position.x) && handsAnchor.sequence==cameraAnchor.sequence,"interleaving cannot change the hand anchor");
    check(!near(legacySecondRead.position.x,cameraAnchor.position.x),"fixture exposes the former two-read disagreement");
    check(near(cameraSnapshot.latest.root[12],10.12f),"captured matrices do not alias mutable history");
    check(cameraSnapshot.latest.timeMs==1011.11,"receipt timestamp remains paired with the captured data");
    const auto currentAnchor=AnchorFor(cameraSnapshot.Select(false),false);
    check(near(currentAnchor.position.z,31.5f) && !currentAnchor.root,"bone mode preserves camera-bone origin");
    auto seq=history.Capture().latest.sequence;
    check(history.Capture().latest.sequence==seq,"reads never advance simulation history");
    history.Publish({1,2,3},root,3,2,1033.33);
    check(!history.Capture().previous.valid,"new skeleton does not reuse old-owner history");
    check(history.Capture().latest.sequence>seq,"owner changes retain monotonic sequence identity");
    history.Publish({1,2,3},root,3,4,1044.44);
    check(!history.Capture().previous.valid,"new graphics owner also clears previous pose");
    check(!history.Capture().MatchCamera({10,20,31.5f},1050).valid,"owner change discards older camera matches too");

    // Six GPU-captured jeep frames, 2025..2030. The draw's player root matched
    // these native-camera samples. Five frames used the wrong legacy previous
    // seat; one already had the correct previous seat. Matching must select
    // either latest or previous depending on worker timing, not a fixed lag.
    const Vec3 cameras[]={
        {2512.503418f,1197.251343f,22.208748f},{2512.552490f,1197.308716f,22.205154f},
        {2512.597900f,1197.364624f,22.201567f},{2512.648193f,1197.422607f,22.198320f},
        {2512.790283f,1197.580811f,22.189598f},{2512.840576f,1197.633423f,22.187149f},
        {2512.889160f,1197.685059f,22.184753f}};
    const Vec3 seats[]={
        {2512.405029f,1197.277222f,20.832247f},{2512.453857f,1197.335327f,20.828611f},
        {2512.500244f,1197.391968f,20.824936f},{2512.550537f,1197.450439f,20.821535f},
        {2512.697021f,1197.608276f,20.812443f},{2512.748291f,1197.660156f,20.809942f},
        {2512.799072f,1197.710815f,20.807457f}};
    const int newest[]={1,2,4,4,5,6};
    int legacyFailures=0;
    for (int f=0;f<6;++f) {
        BodyPoseHistory replay;
        for (int i=0;i<=newest[f];++i) {
            root[12]=seats[i].x; root[13]=seats[i].y; root[14]=seats[i].z;
            replay.Publish(cameras[i],root,1,2,2000+i*11.11);
        }
        auto snapshot=replay.Capture();
        auto match=snapshot.MatchCamera(cameras[f+1],2100);
        check(match.valid && near(match.root[12],seats[f+1].x) && near(match.root[13],seats[f+1].y),"captured frame selects the root actually sent to GPU");
        if (!near(snapshot.previous.root[12],seats[f+1].x)) ++legacyFailures;
    }
    check(legacyFailures==5,"capture fixture exposes all five old camera/scene mismatches");

    BodyPoseHistory bounded;
    for (int i=0;i<10;++i) { root[12]=static_cast<float>(i); bounded.Publish({static_cast<float>(i),0,1},root,1,2,3000+i*10); }
    check(bounded.Capture().MatchCamera({2,0,1},3100).valid,"oldest retained update can match");
    check(!bounded.Capture().MatchCamera({1,0,1},3100).valid,"discarded history is not invented");
    check(!bounded.Capture().MatchCamera({9.01f,0,1},3100).valid,"no nearest-seat extrapolation beyond one millimetre");
    check(!bounded.Capture().MatchCamera({9,0,1},3400).valid,"stale match is unavailable");
    check(!bounded.Capture().MatchCamera({9,0,1},3089).valid,"future sample is unavailable");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    check(!bounded.Capture().MatchCamera({nan,0,1},3100).valid,"nonfinite native camera rejected");
    check(!bounded.Capture().MatchCamera({9,0,1},nan).valid,"nonfinite clock rejected");
    root[12]=9;
    bounded.Publish({9,0,1},root,1,2,3100);
    check(bounded.Capture().MatchCamera({9,0,1},3110).sequence==11,"identical stationary camera and seat retain newest update");
    root[12]=9.1f;
    bounded.Publish({9,0,1},root,1,2,3110);
    check(!bounded.Capture().MatchCamera({9,0,1},3120).valid,"ambiguous camera with different seats falls back");
    root[0]=nan;
    bounded.Publish({10,0,1},root,1,2,3120);
    check(!bounded.Capture().MatchCamera({10,0,1},3130).valid,"nonfinite seat rejected");
    std::printf("body snapshot interleaving/owner tests: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
