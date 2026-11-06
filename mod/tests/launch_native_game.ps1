# Launch an already-installed game/mod without changing any game binary.
$ErrorActionPreference='Stop'
if(Get-Process FarCry2,FarCry2VR -ErrorAction SilentlyContinue){throw 'Game already running'}
$trial=Get-Content (Join-Path $PSScriptRoot 'test-manifest.json') -Raw|ConvertFrom-Json
$exe=$trial.executable
if(!$exe){$exe=Join-Path $trial.bin 'FarCry2VR.exe';if(!(Test-Path -LiteralPath $exe)){$exe=Join-Path $trial.bin 'FarCry2.exe'}}
if(!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath $trial.xr_runtime)){throw 'Executable or 32-bit OpenXR runtime manifest is missing'}
foreach($b in $trial.original_binaries){if((Get-FileHash -LiteralPath $b.path).Hash -ne $b.sha256){throw 'Original binary changed'}}
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NativeVrLaunch {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct Startup { public int cb;public string reserved,desktop,title;public int x,y,w,h,xc,yc,fill,flags;public short show,extra;public IntPtr extraPtr,input,output,error; }
 [StructLayout(LayoutKind.Sequential)] public struct ProcessInfo { public IntPtr process,thread;public uint pid,tid; }
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool CreateProcess(string app,StringBuilder args,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup startup,out ProcessInfo process);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GetProcessAffinityMask(IntPtr process,out UIntPtr allowed,out UIntPtr system);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern bool SetProcessAffinityMask(IntPtr process,UIntPtr mask);
 [DllImport("kernel32.dll")] public static extern uint ResumeThread(IntPtr thread);
 [DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr process,uint status);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
$si=New-Object NativeVrLaunch+Startup;$si.cb=[Runtime.InteropServices.Marshal]::SizeOf($si)
$pi=New-Object NativeVrLaunch+ProcessInfo;$running=$false;$oldRuntime=$env:XR_RUNTIME_JSON
try{
 $env:XR_RUNTIME_JSON=$trial.xr_runtime
 if(![NativeVrLaunch]::CreateProcess($exe,[Text.StringBuilder]::new(('"'+$exe+'" -GameProfile_SkipIntroMovies 1')),[IntPtr]::Zero,[IntPtr]::Zero,$false,4,[IntPtr]::Zero,$trial.bin,[ref]$si,[ref]$pi)){throw 'CreateProcess failed'}
 $allowed=[UIntPtr]::Zero;$system=[UIntPtr]::Zero
 if(![NativeVrLaunch]::GetProcessAffinityMask($pi.process,[ref]$allowed,[ref]$system)){throw 'Affinity query failed'}
 [uint64]$mask=0;$cores=0
 for($bit=0;$bit -lt 64 -and $cores -lt 16;$bit++){[uint64]$flag=[uint64]1 -shl $bit;if($allowed.ToUInt64() -band $flag){$mask=$mask -bor $flag;$cores++}}
 if(!$mask -or ![NativeVrLaunch]::SetProcessAffinityMask($pi.process,[UIntPtr]::new($mask))){throw 'Affinity setup failed'}
 if([NativeVrLaunch]::ResumeThread($pi.thread) -eq [uint32]::MaxValue){throw 'Resume failed'}
 $running=$true
 @{pid=$pi.pid;started=[DateTime]::UtcNow.ToString('o');exe=$exe;runtime=$trial.xr_runtime;core_limit=$cores}|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'launch.json')
 "Launched owned Far Cry 2 process $($pi.pid), affinity cores $cores"
}finally{
 $env:XR_RUNTIME_JSON=$oldRuntime
 if($pi.process -ne [IntPtr]::Zero){if(!$running){[void][NativeVrLaunch]::TerminateProcess($pi.process,1)};[void][NativeVrLaunch]::CloseHandle($pi.process)}
 if($pi.thread -ne [IntPtr]::Zero){[void][NativeVrLaunch]::CloseHandle($pi.thread)}
}
