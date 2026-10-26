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
void Unavailable(const char* reason);
bool Raw(xr::HandState hands[2],const XrPosef& head,bool headTracked); // true -> normal input-generation invalidation
void Published(const xr::InputStamp& stamp,const xr::HandState hands[2]);
void Polled(const xr::InputStamp& source,std::uint64_t ownerEpoch,unsigned triggerByte,float triggerValue);
struct PoseRecord {
    xr::InputStamp source{};
    std::uint64_t ownerEpoch=0,appFrame=0,observedMs=0;
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
