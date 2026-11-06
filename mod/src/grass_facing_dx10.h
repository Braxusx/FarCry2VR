#pragma once
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "common/grass_facing.h"

// Reflection qualifies the provider layout; buffer creation clears recycled
// pointer identities. No shader bytecode, vertex data or view matrix is patched.
namespace grass_facing {
struct Shader { void* shader;UINT slot; };
struct Buffer { ID3D10Buffer* buffer;bool qualified; };
inline SRWLOCK lock=SRWLOCK_INIT;
inline std::vector<Shader> shaders;
inline std::vector<Buffer> buffers;
inline bool enabled=false,active=false;
inline float headYaw=0,trackingAnchor=0,targetYaw=0;
inline bool haveAnchor=false;
inline bool Enabled(){return enabled;}
inline void Configure(bool value){enabled=value;}
inline void Pose(float yaw,float worldYaw,bool valid){
    AcquireSRWLockExclusive(&lock);active=enabled && valid;
    // Mod recenter changes its reference, but must not spin grass back to zero.
    if(active){
        if(!haveAnchor){trackingAnchor=yaw;haveAnchor=true;}
        headYaw=fc2vr::body_yaw::Wrap(yaw-trackingAnchor);
        // The native upload can contain an older camera orientation. Derive a
        // world target from the rendered pose, rather than subtracting today's
        // head yaw from yesterday's billboard matrix.
        targetYaw=fc2vr::body_yaw::Wrap(worldYaw-headYaw-fc2vr::body_yaw::Pi/2);
    }
    ReleaseSRWLockExclusive(&lock);
}
inline void Created(ID3D10Buffer* b){
    if(!enabled)return;
    AcquireSRWLockExclusive(&lock);
    for(auto& item:buffers)if(item.buffer==b){item.qualified=false;ReleaseSRWLockExclusive(&lock);return;}
    if(buffers.size()<4096)buffers.push_back({b,false});
    ReleaseSRWLockExclusive(&lock);
}
inline void ShaderCreated(void* shader,const void* code,SIZE_T size){
    if(!enabled)return;
    AcquireSRWLockExclusive(&lock);
    shaders.erase(std::remove_if(shaders.begin(),shaders.end(),[&](const Shader& s){return s.shader==shader;}),shaders.end());
    ReleaseSRWLockExclusive(&lock);
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    static HMODULE module=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    static Reflect reflect=module?(Reflect)GetProcAddress(module,"D3DReflect"):nullptr;
    if(!reflect)return;
    ID3D11ShaderReflection* r=nullptr;
    // IID from the Windows SDK d3d11shader.h (MinGW does not emit __uuidof).
    constexpr GUID reflectionIID{0x8d536ca1,0x0cca,0x4956,{0xa8,0x37,0x78,0x69,0x63,0x75,0x55,0x84}};
    if(FAILED(reflect(code,size,reflectionIID,(void**)&r)) || !r)return;
    auto* cb=r->GetConstantBufferByName("ViewportShaderParameterProvider");
    if(!cb){r->Release();return;}
    D3D11_SHADER_BUFFER_DESC bd{};D3D11_SHADER_VARIABLE_DESC vd{};D3D11_SHADER_TYPE_DESC td{};
    auto* var=cb->GetVariableByName("_GrassCylindricalBillboardMatrix");
    if(!var){r->Release();return;}
    bool ok=SUCCEEDED(cb->GetDesc(&bd)) && bd.Type==D3D_CT_CBUFFER && bd.Size==1008 &&
        SUCCEEDED(var->GetDesc(&vd)) && (vd.uFlags&D3D_SVF_USED) && vd.StartOffset==512 && vd.Size==64 &&
        SUCCEEDED(var->GetType()->GetDesc(&td)) && td.Type==D3D_SVT_FLOAT && td.Rows==4 && td.Columns==4;
    D3D11_SHADER_DESC sd{};UINT slot=16;
    if(ok && SUCCEEDED(r->GetDesc(&sd)))for(UINT i=0;i<sd.BoundResources;++i){
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if(SUCCEEDED(r->GetResourceBindingDesc(i,&binding)) && binding.Type==D3D_SIT_CBUFFER && binding.BindCount==1 && !strcmp(binding.Name,"ViewportShaderParameterProvider"))slot=binding.BindPoint;
    }
    if(ok && slot<16){AcquireSRWLockExclusive(&lock);shaders.push_back({shader,slot});ReleaseSRWLockExclusive(&lock);}
    r->Release();
}
inline void Bound(void* shader,UINT start,UINT count,ID3D10Buffer* const* b){
    if(!enabled || !b)return;
    AcquireSRWLockExclusive(&lock);
    for(const auto& s:shaders)if(s.shader==shader && s.slot>=start && s.slot-start<count){
        auto* target=b[s.slot-start];if(!target)break;
        D3D10_BUFFER_DESC desc{};target->GetDesc(&desc);
        if(desc.ByteWidth!=1008 || !(desc.BindFlags&D3D10_BIND_CONSTANT_BUFFER))break;
        bool found=false;
        for(auto& item:buffers)if(item.buffer==target){item.qualified=true;found=true;break;}
        if(!found && buffers.size()<4096)buffers.push_back({target,true});
        break;
    }
    ReleaseSRWLockExclusive(&lock);
}
inline bool Patch(ID3D10Buffer* buffer,void* data){
    if(!enabled || !data)return false;
    bool qualified=false;float yaw=0,target=0;
    AcquireSRWLockShared(&lock);
    if(active){for(const auto& item:buffers)if(item.buffer==buffer){qualified=item.qualified;break;}yaw=headYaw;target=targetYaw;}
    ReleaseSRWLockShared(&lock);
    if(!qualified)return false;
    auto* matrix=(float*)((BYTE*)data+512);
    const float before=atan2f(matrix[4],matrix[0]);
    if(!fc2vr::grass::SetWorldYaw(matrix,target))return false;
    static uint64_t last=0;const auto now=GetTickCount64();
    if(now-last>1000){last=now;LOG("grass-facing head=%.6f native=%.6f corrected=%.6f",yaw,before,atan2f(matrix[4],matrix[0]));}
    return true;
}
}
