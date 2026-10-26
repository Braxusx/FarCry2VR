#pragma once
#ifndef FC2VR_SCENARIO_TESTING
#define FC2VR_SCENARIO_TESTING 0
#endif
#if FC2VR_SCENARIO_TESTING != 0 && FC2VR_SCENARIO_TESTING != 1
#error FC2VR_SCENARIO_TESTING must be 0 or 1
#endif
#if FC2VR_SCENARIO_TESTING
#include "xr.h"
#include <cstdint>
namespace scenario_debug {
// Only the already-verified startup path may configure this debug-only adapter.
void Configure(const char* modDirectory);
void Runtime(const char* name,std::uint64_t version);
void RigSettings(int ik,int named,float limit);
struct FrameRecord {
    std::uint64_t appFrame=0,space=0;
    std::int64_t displayTime=0;
    float worldScale=0,heightOffset=0,renderScale=0,overscan[2]{};
    int steadyCamera=0,fovMode=0,cullMode=0;
    bool stereo=false,afw=false,dlss=false;
    float reference[9]{},fov[8]{},eyes[14]{};
};
void Frame(const FrameRecord& frame); // source-owned render context; no game reads
void Unavailable(const char* reason);
bool Raw(xr::HandState hands[2],const XrPosef& head,bool headTracked); // true -> normal input-generation invalidation
void Published(const xr::InputStamp& stamp,const xr::HandState hands[2]);
void Polled(const xr::InputStamp& source,std::uint64_t ownerEpoch,unsigned triggerByte,float triggerValue);
struct PoseRecord {
    xr::InputStamp source{};
    std::uint64_t ownerEpoch=0,appFrame=0,observedMs=0,rig=0;
    bool namedRig=false,physical=false;
    int indices[6]{-1,-1,-1,-1,-1,-1};
    float model[6][16]{}; // bounded copy of actual post-writeback palette records
    float world[16]{}; // same model-to-world frame used by that Apply call
};
void Applied(const PoseRecord& pose);
void CaptureEye(ID3D10Device* device,ID3D10Texture2D* texture,unsigned eye,std::uint64_t appFrame);
bool Active(); // suppress persistent hold learning/INI writes during stimulus/drain
}
#endif
