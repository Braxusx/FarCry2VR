param(
    [switch]$ScenarioTesting,
    [string]$Compiler='C:/msys64/mingw32/bin/i686-w64-mingw32-g++.exe'
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$flavor=if($ScenarioTesting) {'startup-camera-1-debug'} else {'startup-camera-1-production'}
$out=Join-Path $repo ('build/'+$flavor)
if(!(Test-Path -LiteralPath $Compiler -PathType Leaf)) { throw "Compiler not found: $Compiler" }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$compilerDir=Split-Path -Parent $Compiler
$previousPath=$env:PATH
Push-Location -LiteralPath $repo
try {
    $env:PATH=$compilerDir+';'+$previousPath
    $compileArgs=@('-std=c++17','-O2','-Wall','-Wno-unused-function','-Wno-missing-braces','-I','mod/third_party/openxr_include')
    if($ScenarioTesting) { $compileArgs+='-DFC2VR_SCENARIO_TESTING=1' }
    $compileArgs+=@('-shared','-o',(Join-Path $out 'dinput8.dll'))
    $compileArgs+=@('main','xr','perf','hang','controls','arms','dlss_fc2','dlss_client','afw_fc2','gameaddr','diag','scenario_runner') | ForEach-Object { 'mod/src/'+$_+'.cpp' }
    $compileArgs+=@('-static','-static-libgcc','-static-libstdc++','-Wl,--kill-at','-Wl,--enable-stdcall-fixup','-ld3d11','-ldxgi','-lwinmm','-luuid','-lgdi32')
    & $Compiler @compileArgs *> (Join-Path $out 'build.log')
    $buildExit=$LASTEXITCODE
    if($buildExit -ne 0) { Get-Content -LiteralPath (Join-Path $out 'build.log');throw "Build failed: $buildExit" }
    $dll=Join-Path $out 'dinput8.dll'
    $bytes=[IO.File]::ReadAllBytes($dll)
    if($bytes.Length -lt 256) { throw 'No usable PE output' }
    $nt=[BitConverter]::ToUInt32($bytes,60)
    if($nt -gt $bytes.Length-26 -or [BitConverter]::ToUInt16($bytes,$nt+4) -ne 0x14c -or [BitConverter]::ToUInt16($bytes,$nt+24) -ne 0x10b) { throw 'Expected PE32/i386' }
    $strings=[Text.Encoding]::ASCII.GetString($bytes)
    $wideStrings=[Text.Encoding]::Unicode.GetString($bytes)
    foreach($token in @('FC2VR_SCENARIO_ARM','ALLOW_SYNTHETIC_INPUT','FC2VR_SCENARIO_FIXTURE','FC2VR_SCENARIO_SAVE','FC2VR_SCENARIO_PROFILE','scenario_debug')) {
        $present=$strings.Contains($token) -or $wideStrings.Contains($token)
        if($present -ne $ScenarioTesting.IsPresent) { throw "Scenario compile-gate mismatch: $token" }
    }
    $objdump=Join-Path $compilerDir 'objdump.exe'
    if(!(Test-Path -LiteralPath $objdump)) { $objdump=Join-Path $compilerDir 'i686-w64-mingw32-objdump.exe' }
    if(!(Test-Path -LiteralPath $objdump)) { throw 'objdump required to verify the proxy export' }
    & $objdump -p $dll *> (Join-Path $out 'pe.log')
    if($LASTEXITCODE -ne 0) { throw 'PE inspection failed' }
    if(!(Select-String -LiteralPath (Join-Path $out 'pe.log') -Pattern '\sDirectInput8Create\s*$')) { throw 'Proxy export missing' }
    Write-Output ('Built '+$flavor+'; compile-gated scenario testing='+[int]$ScenarioTesting.IsPresent)
    Write-Output 'Verified PE32/i386, DirectInput8Create and scenario payload inclusion/exclusion.'
    Get-FileHash -LiteralPath $dll -Algorithm SHA256 | Format-List
} finally { Pop-Location;$env:PATH=$previousPath }
# Build only. This script never launches, injects, deploys, configures XR or packages a release.
