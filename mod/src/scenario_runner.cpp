#include "scenario_runner.h"
#if FC2VR_SCENARIO_TESTING
#include "common/scenario.h"
#include "common/scenario_capture.h"
#include "common/image_identity.h"
#include "arms.h"
#include "controls.h"
#include "log.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace scenario_debug {
namespace {
using namespace fc2vr::scenario;
Runner runner;
Case selected=Case::None;
SRWLOCK feedbackLock=SRWLOCK_INIT;
Observation feedback;
PoseRecord latestPose;
xr::InputStamp publishedSource;
Decision decision;
std::atomic<bool> active{false};
bool configured=false,started=false,ioOk=true,anchored=false,reported=false;
XrPosef anchor{{0,0,0,1},{0,0,0}};
char root[MAX_PATH]{},directory[MAX_PATH]{};
HANDLE trace=INVALID_HANDLE_VALUE;
EyePair pair;
std::uint64_t capturedTicket=0,captureFrame=0,attemptTicket=0;
unsigned attempts=0;

bool Write(HANDLE file,const void* data,std::size_t bytes) {
    auto* p=static_cast<const std::uint8_t*>(data);
    while(bytes) { const DWORD want=static_cast<DWORD>(std::min<std::size_t>(bytes,0x100000));DWORD n=0;
        if(!WriteFile(file,p,want,&n,nullptr)||!n) return false;
        p+=n;bytes-=n; }
    return true;
}
void Line(const char* format,...) {
    if(trace==INVALID_HANDLE_VALUE || !ioOk) return;
    char text[16384];va_list args;va_start(args,format);const int n=vsnprintf(text,sizeof text,format,args);va_end(args);
    ioOk=n>0&&n<static_cast<int>(sizeof text)&&Write(trace,text,static_cast<std::size_t>(n));
    if(!ioOk) LOG("scenario-runner-1: trace write failed; stimulus will abort");
}
bool OpenRun() {
    const int n=snprintf(directory,sizeof directory,"%s\\scenario-%lu-%llu",root,GetCurrentProcessId(),static_cast<unsigned long long>(GetTickCount64()));
    if(n<0||n>=static_cast<int>(sizeof directory)-80 || !CreateDirectoryA(directory,nullptr)) return false;
    char path[MAX_PATH];snprintf(path,sizeof path,"%s\\trace.jsonl",directory);
    trace=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(trace==INVALID_HANDLE_VALUE) return false;
    Line("{\"event\":\"begin\",\"version\":\"scenario-runner-1\",\"case\":\"%s\",\"environment\":\"in_game\",\"synthetic_input\":true,\"headset_accepted\":false,\"fixture\":\"head-relative-v1\"}\n",Name(selected));
    LOG("scenario-runner-1: case=%s output=%s SYNTHETIC INPUT; stop with abort.txt in that folder",Name(selected),directory);
    return ioOk;
}
void Terminal() {
    if(reported || runner.Status()==State::Waiting || runner.Status()==State::Running || runner.Status()==State::Idle) return;
    reported=true;
    Line("{\"event\":\"end\",\"case\":\"%s\",\"observed_complete\":%s,\"reason\":\"%s\",\"visual_review\":\"required\",\"headset_accepted\":false}\n",Name(selected),runner.Status()==State::Complete?"true":"false",runner.Reason());
    if(trace!=INVALID_HANDLE_VALUE) { FlushFileBuffers(trace);CloseHandle(trace);trace=INVALID_HANDLE_VALUE; }
    LOG("scenario-runner-1: end case=%s complete=%d reason=%s (not visual/headset acceptance)",Name(selected),runner.Status()==State::Complete,runner.Reason());
}
bool AbortFile() { char p[MAX_PATH];snprintf(p,sizeof p,"%s\\abort.txt",directory);return GetFileAttributesA(p)!=INVALID_FILE_ATTRIBUTES; }
XrPosef InAnchor(const Pose& p) {
    const Quat q{anchor.orientation.x,anchor.orientation.y,anchor.orientation.z,anchor.orientation.w};
    const auto r=Rotate(q,p.p);const auto o=Mul(q,p.q);
    return {{o.x,o.y,o.z,o.w},{anchor.position.x+r.x,anchor.position.y+r.y,anchor.position.z+r.z}};
}
bool HeadStationary(const XrPosef& p) {
    const float x=p.position.x-anchor.position.x,y=p.position.y-anchor.position.y,z=p.position.z-anchor.position.z;
    const float dot=std::fabs(p.orientation.x*anchor.orientation.x+p.orientation.y*anchor.orientation.y+p.orientation.z*anchor.orientation.z+p.orientation.w*anchor.orientation.w);
    return x*x+y*y+z*z<=0.01f && dot>=0.9961947f; // 10 cm / 10 degrees; eyes themselves are never overridden
}
}
void Configure(const char*) {
    if(configured) return;
    configured=true;
    char name[32]{},arm[64]{};
    GetEnvironmentVariableA("FC2VR_SCENARIO",name,sizeof name);
    GetEnvironmentVariableA("FC2VR_SCENARIO_ARM",arm,sizeof arm);
    const DWORD n=GetEnvironmentVariableA("FC2VR_SCENARIO_OUT",root,sizeof root);
    selected=ParseCase(name);
    const bool absolute=(root[0]&&root[1]==':'&&(root[2]=='\\'||root[2]=='/'))||(root[0]=='\\'&&root[1]=='\\');
    if(selected==Case::None || std::strcmp(arm,"ALLOW_SYNTHETIC_INPUT") || !n || n>=sizeof root || !absolute
        || GetFileAttributesA(root)==INVALID_FILE_ATTRIBUTES || !(GetFileAttributesA(root)&FILE_ATTRIBUTE_DIRECTORY)) selected=Case::None;
    LOG("scenario-runner-1: debug build; configured=%s; production authority gates retained",Name(selected));
}
bool Active() { return active.load(); }
void Unavailable(const char* why) {
    if(!started) return;
    runner.Abort(why); // XR/render-thread entry points only; feedback callbacks never mutate the plan
    active.store(runner.Busy()||decision.overrideInput);
    Terminal(); // retain a failure receipt even if the XR session never resumes
}
bool Raw(xr::HandState hands[2],const XrPosef& head,bool headTracked) {
    if(selected==Case::None) return false;
    if(!started) { started=true;if(!OpenRun()) { selected=Case::None;LOG("scenario-runner-1: output unavailable; not armed");return false; }runner.Start(selected,GetTickCount64()); }
    const auto owner=arms::CurrentOwner();
    Observation o;AcquireSRWLockShared(&feedbackLock);o=feedback;ReleaseSRWLockShared(&feedbackLock);
    const auto source=xr::CurrentInputStamp();
    o.ownerEpoch=owner.epoch;o.weapon=owner.key.weapon;
    o.safe=owner.valid&&headTracked&&Unit({head.orientation.x,head.orientation.y,head.orientation.z,head.orientation.w})&&Finite({head.position.x,head.position.y,head.position.z})&&controls::NormalControls()&&controls::VehicleMode()==0&&!controls::MapOpen()&&!controls::MonocularAtFace();
    if(!ioOk || AbortFile()) runner.Abort(ioOk?"abort-file":"trace-failed");
    if(headTracked&&anchored&&!HeadStationary(head)) runner.Abort("head-moved");
    if(o.safe&&!anchored) { anchor=head;anchored=true;Line("{\"event\":\"anchor\",\"position\":[%.9g,%.9g,%.9g],\"orientation\":[%.9g,%.9g,%.9g,%.9g]}\n",head.position.x,head.position.y,head.position.z,head.orientation.x,head.orientation.y,head.orientation.z,head.orientation.w); }
    decision=runner.Tick(GetTickCount64(),source.sequence+1,o);
    active.store(runner.Busy()||decision.overrideInput); // includes the final neutral frame, so it cannot be learned
    if(decision.boundary) { arms::ScenarioBoundary();controls::CancelPendingInput(); }
    if(decision.overrideInput) for(unsigned h=0;h<2;++h) {
        // First remove real buttons, never combine an unattended script with physical fire/locomotion.
        hands[h].trigger=hands[h].squeeze=0;hands[h].stick={};hands[h].stickClick=hands[h].btn1=hands[h].btn2=hands[h].menu=false;
        if(decision.syntheticPose) {
            const auto& scripted=decision.hand[h];hands[h].active=true;
            hands[h].grip=InAnchor(scripted.grip);hands[h].aim=InAnchor(scripted.aim);
            hands[h].gripEvidence=hands[h].aimEvidence={true,scripted.tracked,scripted.tracked,scripted.tracked,scripted.tracked};
            hands[h].trigger=scripted.trigger;hands[h].btn2=scripted.select;
        }
    }
    Terminal();return decision.boundary;
}
void Published(const xr::InputStamp& source,const xr::HandState hands[2]) {
    if(!started || !decision.overrideInput) return;
    PoseRecord pose;Observation observed;
    AcquireSRWLockExclusive(&feedbackLock);
    publishedSource=source;feedback.sequence=source.sequence;feedback.handEpoch=source.handGeneration[1];
    feedback.rightAuthority=hands[1].poseAuthority;feedback.rightTracked=hands[1].gripEvidence.Tracked()&&hands[1].aimEvidence.Tracked();pose=latestPose;observed=feedback;
    ReleaseSRWLockExclusive(&feedbackLock);
    Line("{\"event\":\"input\",\"ticket\":%llu,\"step\":\"%s\",\"sequence\":%llu,\"generation\":%llu,\"hand_epoch\":%llu,\"first_sequence\":%llu,\"tracked\":%d,\"authority\":%d,\"synthetic_pose\":%d,\"right_grip\":[%.9g,%.9g,%.9g],\"right_rotation\":[%.9g,%.9g,%.9g,%.9g],\"applied_sequence\":%llu,\"applied_owner\":%llu,\"polled_sequence\":%llu,\"polled_owner\":%llu,\"emitted_trigger\":%.9g}\n",
        static_cast<unsigned long long>(decision.ticket),decision.label,static_cast<unsigned long long>(source.sequence),static_cast<unsigned long long>(source.generation),static_cast<unsigned long long>(source.handGeneration[1]),static_cast<unsigned long long>(decision.firstSequence),hands[1].gripEvidence.Tracked(),hands[1].poseAuthority,decision.syntheticPose,
        hands[1].grip.position.x,hands[1].grip.position.y,hands[1].grip.position.z,hands[1].grip.orientation.x,hands[1].grip.orientation.y,hands[1].grip.orientation.z,hands[1].grip.orientation.w,
        static_cast<unsigned long long>(pose.source.sequence),static_cast<unsigned long long>(pose.ownerEpoch),static_cast<unsigned long long>(observed.polledSequence),static_cast<unsigned long long>(observed.polledOwner),observed.emittedTrigger);
}
void Polled(const xr::InputStamp& source,std::uint64_t epoch,unsigned byte,float value) {
    if(!active.load()) return;
    AcquireSRWLockExclusive(&feedbackLock);feedback.polledSequence=source.sequence;feedback.polledOwner=epoch;
    feedback.polledMs=GetTickCount64();feedback.emittedTrigger=byte?float(byte)/255.0f:value;ReleaseSRWLockExclusive(&feedbackLock);
}
void Applied(const PoseRecord& pose) {
    if(!active.load()) return;
    AcquireSRWLockExclusive(&feedbackLock);latestPose=pose;feedback.appliedSequence=pose.source.sequence;
    feedback.appliedOwner=pose.ownerEpoch;feedback.appliedMs=pose.observedMs;ReleaseSRWLockExclusive(&feedbackLock);
}
void CaptureEye(ID3D10Device* device,ID3D10Texture2D* texture,unsigned eye,std::uint64_t appFrame) {
    if(!decision.capture || runner.Status()!=State::Running || !active.load() || !device || !texture || eye>1 || capturedTicket==decision.ticket) return;
    xr::InputStamp source;PoseRecord pose;Observation observed;
    AcquireSRWLockShared(&feedbackLock);source=publishedSource;pose=latestPose;observed=feedback;ReleaseSRWLockShared(&feedbackLock);
    const auto owner=arms::CurrentOwner();
    if(!owner.valid || source.sequence<decision.firstSequence || !xr::HeadCurrent(source)
        || observed.polledSequence<decision.firstSequence || observed.polledOwner!=owner.epoch || observed.polledSequence>source.sequence) return;
    if(pair.Has(eye,decision.ticket,appFrame,source.sequence,owner.epoch)) return;
    const auto check=Plan(selected,decision.step).check;
    const bool needsPose=check==Check::Ready||check==Check::Pose||check==Check::Swap||check==Check::Recovered;
    if(needsPose && (!pose.physical || pose.source.sequence<decision.firstSequence || pose.source.sequence>source.sequence || pose.appFrame>appFrame || pose.ownerEpoch!=owner.epoch || !xr::InputCurrent(pose.source,1))) return;
    if(attemptTicket!=decision.ticket) { attemptTicket=decision.ticket;captureFrame=0;attempts=0;pair.Clear(); }
    if(captureFrame!=appFrame) { captureFrame=appFrame;if(++attempts>4) { Unavailable("capture-pair-missing");return; } }
    D3D10_TEXTURE2D_DESC d;texture->GetDesc(&d);
    const auto format=d.Format==DXGI_FORMAT_R8G8B8A8_UNORM||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?Pixels::Rgba8:
        d.Format==DXGI_FORMAT_B8G8R8A8_UNORM||d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?Pixels::Bgra8:
        d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?Pixels::Rgba16f:d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?Pixels::Rgba32f:Pixels::Unsupported;
    if(!d.Width||!d.Height||d.Width>4096||d.Height>4096||d.SampleDesc.Count!=1||format==Pixels::Unsupported) { Unavailable("capture-format-unavailable");return; }
    d.Usage=D3D10_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D10_CPU_ACCESS_READ;d.MiscFlags=0;d.MipLevels=1;d.ArraySize=1;
    ID3D10Texture2D* staging=nullptr;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&staging))) { Unavailable("capture-staging-failed");return; }
    device->CopySubresourceRegion(staging,0,0,0,0,texture,0,nullptr);
    D3D10_MAPPED_TEXTURE2D mapped{};std::vector<std::uint8_t> bmp;
    if(SUCCEEDED(staging->Map(0,D3D10_MAP_READ,0,&mapped))) {
        try { Bmp(static_cast<const std::uint8_t*>(mapped.pData),std::size_t(mapped.RowPitch)*d.Height,d.Width,d.Height,mapped.RowPitch,format,bmp); }
        catch(const std::bad_alloc&) { bmp.clear(); }
        staging->Unmap(0);
    }
    staging->Release();
    if(bmp.empty()) { Unavailable("capture-encode-failed");return; }
    char file[96],path[MAX_PATH];snprintf(file,sizeof file,"step-%llu-frame-%llu-eye%u.bmp",static_cast<unsigned long long>(decision.ticket),static_cast<unsigned long long>(appFrame),eye);
    snprintf(path,sizeof path,"%s\\%s",directory,file);
    HANDLE output=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    const bool written=output!=INVALID_HANDLE_VALUE&&Write(output,bmp.data(),bmp.size());
    if(output!=INVALID_HANDLE_VALUE) { FlushFileBuffers(output);CloseHandle(output); }
    if(!written) { Unavailable("capture-write-failed");return; }
    fc2vr::image::Sha256 hash;fc2vr::image::Digest digest{};char hex[65];hash.Update(bmp.data(),bmp.size());hash.Finish(digest);fc2vr::image::Hex(digest,hex);
    Line("{\"event\":\"capture\",\"ticket\":%llu,\"step\":\"%s\",\"file\":\"%s\",\"sha256\":\"%s\",\"eye\":%u,\"app_frame\":%llu,\"sequence\":%llu,\"first_sequence\":%llu,\"owner\":%llu,\"weapon\":%llu,\"pose_sequence\":%llu,\"pose_owner\":%llu,\"pose_app_frame\":%llu,\"pose_physical\":%d,\"named_rig\":%d,\"polled_sequence\":%llu,\"polled_owner\":%llu,\"trigger\":%.9g,\"tracked\":%d,\"authority\":%d,\"hand_epoch\":%llu}\n",
        static_cast<unsigned long long>(decision.ticket),decision.label,file,hex,eye,static_cast<unsigned long long>(appFrame),static_cast<unsigned long long>(source.sequence),static_cast<unsigned long long>(decision.firstSequence),static_cast<unsigned long long>(owner.epoch),static_cast<unsigned long long>(owner.key.weapon),static_cast<unsigned long long>(pose.source.sequence),static_cast<unsigned long long>(pose.ownerEpoch),static_cast<unsigned long long>(pose.appFrame),pose.physical,pose.namedRig,
        static_cast<unsigned long long>(observed.polledSequence),static_cast<unsigned long long>(observed.polledOwner),observed.emittedTrigger,observed.rightTracked,observed.rightAuthority,static_cast<unsigned long long>(observed.handEpoch));
    // The raw matrix copy is evidence of our palette writer, not proof of the GPU skinning consumer.
    char matrices[8192];int used=snprintf(matrices,sizeof matrices,"{\"event\":\"palette\",\"ticket\":%llu,\"eye\":%u,\"app_frame\":%llu,\"sequence\":%llu,\"indices\":[",static_cast<unsigned long long>(decision.ticket),eye,static_cast<unsigned long long>(appFrame),static_cast<unsigned long long>(pose.source.sequence));
    for(int i=0;i<6&&used>0&&used<8000;++i) used+=snprintf(matrices+used,sizeof matrices-used,"%s%d",i?",":"",pose.indices[i]);
    if(used>0&&used<8000) used+=snprintf(matrices+used,sizeof matrices-used,"],\"model\":[");
    for(int i=0;i<6&&used>0&&used<8000;++i) for(int k=0;k<16&&used<8000;++k) used+=snprintf(matrices+used,sizeof matrices-used,"%s%.9g",i||k?",":"",pose.model[i][k]);
    if(used>0&&used<8000) used+=snprintf(matrices+used,sizeof matrices-used,"],\"world\":[");
    for(int k=0;k<16&&used>0&&used<8000;++k) used+=snprintf(matrices+used,sizeof matrices-used,"%s%.9g",k?",":"",pose.world[k]);
    if(used>0&&used<8000) { used+=snprintf(matrices+used,sizeof matrices-used,"]}\n");ioOk=ioOk&&Write(trace,matrices,static_cast<std::size_t>(used)); }
    else ioOk=false;
    if(ioOk&&pair.Add(eye,decision.ticket,appFrame,source.sequence,owner.epoch,true)) {
        capturedTicket=decision.ticket;AcquireSRWLockExclusive(&feedbackLock);feedback.captureTicket=capturedTicket;ReleaseSRWLockExclusive(&feedbackLock);
    }
}
} // namespace scenario_debug
#endif
