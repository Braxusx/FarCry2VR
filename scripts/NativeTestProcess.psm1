# Test-only, no injection. Create suspended so affinity is set BEFORE game code.
# No shell helper, inherited output handles, process-name polling or guessed PID.
Set-StrictMode -Version Latest
if(!('FC2NativeTest.ProcessApi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace FC2NativeTest {
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
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr p,uint ms);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static uint Launch(string exe,string args,string cwd) {
   var si=new SI(); si.cb=Marshal.SizeOf(si); PI pi;
   if(!CreateProcess(exe,new StringBuilder("\""+exe+"\" "+args),IntPtr.Zero,IntPtr.Zero,false,4,IntPtr.Zero,cwd,ref si,out pi)) throw new Win32Exception();
   bool resumed=false;
   try {
    UIntPtr pm,sm;
    if(!GetProcessAffinityMask(pi.process,out pm,out sm)) throw new Win32Exception();
    ulong mask=pm.ToUInt64() & sm.ToUInt64() & 0xffffUL;
    if(mask==0) throw new InvalidOperationException("No usable low-16 affinity bits");
    if(!SetProcessAffinityMask(pi.process,new UIntPtr(mask))) throw new Win32Exception();
    if(ResumeThread(pi.thread)==0xffffffffU) throw new Win32Exception();
    resumed=true; return pi.pid;
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
    $id=[FC2NativeTest.ProcessApi]::Launch($Exe,$Arguments,$WorkingDirectory)
    $p=Get-Process -Id $id -ErrorAction SilentlyContinue
    if(!$p) { throw "Created PID $id exited before identity receipt; never retry automatically" }
    if($p.Path -ne $Exe) { throw 'Created process path mismatch' }
    [pscustomobject]@{Id=$id;Path=$p.Path;StartTicks=$p.StartTime.ToUniversalTime().Ticks;Affinity=$p.ProcessorAffinity.ToInt64()}
}
function Get-OwnedTestProcess {
    param($Identity)
    if(!$Identity) { return $null }
    $p=Get-Process -Id $Identity.Id -ErrorAction SilentlyContinue
    if(!$p) { return $null }
    if($p.Path -ne $Identity.Path -or $p.StartTime.ToUniversalTime().Ticks -ne $Identity.StartTicks) {
        throw 'PID identity changed; refusing to operate on replacement process'
    }
    $p
}
Export-ModuleMember -Function Start-NativeTestProcess,Get-OwnedTestProcess
