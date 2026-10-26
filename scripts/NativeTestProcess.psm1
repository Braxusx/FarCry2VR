# Test-only, no injection. Create suspended so affinity is set BEFORE game code.
# No shell helper, inherited output handles, process-name polling or guessed PID.
Set-StrictMode -Version Latest
if(!('FC2NativeTestV2.ProcessApi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace FC2NativeTestV2 {
 public sealed class Identity {
  public uint Id;
  public string Path;
  public long StartTicks;
  public long Affinity;
 }
 public static class ProcessApi {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct SI { public int cb; public string reserved,desktop,title; public int x,y,w,h,cx,cy,fill,flags; public short show,cbReserved; public IntPtr reserved2,input,output,error; }
  [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr process,thread; public uint pid,tid; }
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
  static extern bool CreateProcess(string app,StringBuilder cmd,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessAffinityMask(IntPtr p,out UIntPtr pm,out UIntPtr sm);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetProcessAffinityMask(IntPtr p,UIntPtr mask);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr t);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr p,uint code);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr p,uint flags,StringBuilder name,ref uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr p,out long created,out long exited,out long kernel,out long user);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint GetProcessId(IntPtr p);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr p,uint ms);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static Identity InspectHandle(IntPtr handle) {
   var name=new StringBuilder(32768); uint size=(uint)name.Capacity;
   long created,exited,kernel,user; UIntPtr pm,sm;
   if(!QueryFullProcessImageName(handle,0,name,ref size) || !GetProcessTimes(handle,out created,out exited,out kernel,out user)
      || !GetProcessAffinityMask(handle,out pm,out sm)) throw new Win32Exception();
   return new Identity {Id=GetProcessId(handle),Path=name.ToString(),StartTicks=DateTime.FromFileTimeUtc(created).Ticks,Affinity=(long)pm.ToUInt64()};
  }
  public static Identity Launch(string exe,string args,string cwd) {
   var si=new SI(); si.cb=Marshal.SizeOf(si); PI pi;
   if(!CreateProcess(exe,new StringBuilder("\""+exe+"\" "+args),IntPtr.Zero,IntPtr.Zero,false,4,IntPtr.Zero,cwd,ref si,out pi)) throw new Win32Exception();
   bool resumed=false;
   try {
    UIntPtr pm,sm;
    if(!GetProcessAffinityMask(pi.process,out pm,out sm)) throw new Win32Exception();
    ulong mask=pm.ToUInt64() & sm.ToUInt64() & 0xffffUL;
    if(mask==0) throw new InvalidOperationException("No usable low-16 affinity bits");
    if(!SetProcessAffinityMask(pi.process,new UIntPtr(mask))) throw new Win32Exception();
    // Kernel process-handle identity is available while still suspended; the
    // managed MainModule/PEB path may be unavailable during loader startup.
    var identity=InspectHandle(pi.process);
    if(identity.Id!=pi.pid || !String.Equals(identity.Path,exe,StringComparison.OrdinalIgnoreCase))
      throw new InvalidOperationException("Suspended kernel image identity mismatch");
    if(ResumeThread(pi.thread)==0xffffffffU) throw new Win32Exception();
    resumed=true; return identity;
   } finally {
    // If setup fails this is still the exact suspended process we created.
    if(!resumed) { TerminateProcess(pi.process,1); WaitForSingleObject(pi.process,8000); }
    CloseHandle(pi.thread); CloseHandle(pi.process);
   }
  }
 }
}
'@
}
function Start-NativeTestProcess {
    param([Parameter(Mandatory)][string]$Exe,[string]$Arguments='', [Parameter(Mandatory)][string]$WorkingDirectory)
    $Exe=(Resolve-Path -LiteralPath $Exe).ProviderPath
    $WorkingDirectory=(Resolve-Path -LiteralPath $WorkingDirectory).ProviderPath
    # Launch returns the exact pre-resume identity, even if a short-lived child
    # has exited by the time PowerShell runs. No post-resume throw can lose its PID.
    [FC2NativeTestV2.ProcessApi]::Launch($Exe,$Arguments,$WorkingDirectory)
}
function Get-OwnedTestProcess {
    param($Identity)
    if(!$Identity) { return $null }
    $p=Get-Process -Id $Identity.Id -ErrorAction SilentlyContinue
    if(!$p) { return $null }
    # Bind the managed object's native handle BEFORE inspecting it. Close/Kill
    # then operate on that handle, not a newly opened PID after a reuse window.
    try { $current=[FC2NativeTestV2.ProcessApi]::InspectHandle($p.Handle) }
    catch { try {$p.Refresh();$gone=$p.HasExited} finally {$p.Dispose()};if($gone) { return $null };throw }
    if($current.Id -ne $Identity.Id -or $current.Path -ne $Identity.Path -or $current.StartTicks -ne $Identity.StartTicks) {
        $p.Dispose()
        throw 'PID identity changed; refusing to operate on replacement process'
    }
    $p
}
function Read-NativeTestText {
    param([Parameter(Mandatory)][string]$Path,[int]$MaxBytes=16777216)
    $stream=$null
    try {
        # Unlike File.ReadAllText, allow the native logger/trace writer to keep
        # its write handle open and permit rotation/rename between poll ticks.
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,
            ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $length=$stream.Length
        if($length -gt $MaxBytes) { throw "Telemetry exceeds bounded read limit: $Path" }
        $buffer=[byte[]]::new([int]$length);$offset=0
        while($offset -lt $length) {
            $read=$stream.Read($buffer,$offset,[int]($length-$offset))
            if(!$read) { break };$offset+=$read
        }
        # Read only the initial bounded byte count, even if the writer appends.
        return [Text.Encoding]::UTF8.GetString($buffer,0,$offset)
    } catch [IO.IOException] {
        # Missing, transiently rotated or exclusively locked telemetry is pending,
        # not a native crash. The independent overall watchdog remains in force.
        return $null
    } finally { if($stream) {$stream.Dispose()} }
}
function Test-OwnedTestProcess {
    param($Identity)
    $probe=$null
    try {$probe=Get-OwnedTestProcess $Identity;return [bool]$probe}
    finally {if($probe){$probe.Dispose()}}
}
function Copy-NativeTestFile {
    param([Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Destination)
    for($attempt=0;$attempt -lt 40;++$attempt) {
        try {[IO.File]::Copy($Source,$Destination,$true);return}
        catch [IO.IOException] {if($attempt -eq 39){throw};Start-Sleep -Milliseconds 100}
    }
}
function Get-NativeTestLogFailure {
    param([string]$Text,[bool]$Fresh,[ValidateSet(0,1)][int]$ScenarioTesting=1)
    # Ignore a previous generation even when it contains a crash or a different
    # build marker. A retained log is evidence, not this process's live telemetry.
    if(!$Fresh) { return '' }
    if($Text -match 'FC2VR loaded' -and $Text -notmatch ('startup-camera-1/scenario-runner-2, scenarioTesting='+$ScenarioTesting)) { return 'Wrong loaded mod marker' }
    if($Text -match 'XR: runtime ' -and $Text -notmatch 'XR: runtime xr-sim ') { return 'Wrong runtime; refusing hardware fallback' }
    if($Text -match 'EXCEPTION c0000005') { return 'Native access violation recorded (exit code is not acceptance)' }
    return ''
}
Export-ModuleMember -Function Start-NativeTestProcess,Get-OwnedTestProcess,Test-OwnedTestProcess,Get-NativeTestLogFailure,Read-NativeTestText,Copy-NativeTestFile
