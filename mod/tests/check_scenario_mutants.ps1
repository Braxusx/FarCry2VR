param(
    [string]$Compiler='C:/msys64/mingw32/bin/i686-w64-mingw32-g++.exe',
    [Parameter(Mandatory=$true)][string]$Out
)
# CPU-only tests built from owned source. Never runs a game, XR or graphics app.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$destination=[IO.Path]::GetFullPath($Out)
if(!$destination.StartsWith(($repo+[IO.Path]::DirectorySeparatorChar),[StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be a new directory inside this checkout' }
if(Test-Path -LiteralPath $destination) { throw 'Refusing to overwrite a prior mutant run' }
if(!(Test-Path -LiteralPath $Compiler -PathType Leaf)) { throw 'Compiler unavailable' }
New-Item -ItemType Directory -Path $destination | Out-Null
$previousPath=$env:PATH
try {
    $env:PATH=(Split-Path -Parent $Compiler)+';'+$previousPath
    $test=Join-Path $PSScriptRoot 'test_scenario.cpp'
    $includes=Join-Path $repo 'mod/src'
    $baseline=Join-Path $destination 'baseline.exe'
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Werror' '-I' $includes $test '-o' $baseline
    if($LASTEXITCODE -ne 0) { throw 'GCC baseline compile failed' }
    foreach($group in @('parse','swap','loss','sweep','lifecycle','pair','bitmap')) {
        & $baseline $group
        if($LASTEXITCODE -ne 0) { throw "GCC baseline failed: $group" }
    }
    $mutants=@(
        @{Name='mixed-applied-pose';Header='scenario_capture.h';Old='poseSequence==b.poseSequence&&poseFrame==b.poseFrame&&';New='';Group='pair'},
        @{Name='endpoint-only';Header='scenario.h';Old='return c==Case::ArmSweep&&i>=1&&i<=4?5:c==Case::TrackingLoss&&i>0&&i<4?3:1;';New='(void)c;(void)i;return 1;';Group='sweep'},
        @{Name='old-sample-ack';Header='scenario.h';Old='captureRequested&&o.captureTicket==d.captureId';New='captureRequested&&o.captureTicket!=0';Group='sweep'}
    )
    foreach($mutant in $mutants) {
        $folder=Join-Path $destination $mutant.Name
        New-Item -ItemType Directory -Path (Join-Path $folder 'common') | Out-Null
        $header=Get-Content -LiteralPath (Join-Path $includes ('common/'+$mutant.Header)) -Raw
        if(($header.Split(@($mutant.Old),[StringSplitOptions]::None)).Count -ne 2) { throw 'Mutation must match exactly once' }
        # Generated, isolated test input, never a production-source edit.
        [IO.File]::WriteAllText((Join-Path $folder ('common/'+$mutant.Header)),$header.Replace($mutant.Old,$mutant.New),[Text.UTF8Encoding]::new($false))
        $binary=Join-Path $folder 'mutant.exe'
        & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Werror' '-I' $folder '-I' $includes $test '-o' $binary
        if($LASTEXITCODE -ne 0) { throw "Mutant did not compile: $($mutant.Name)" }
        $witness=& $binary $mutant.Group 2>&1
        $mutationExit=$LASTEXITCODE
        $witness
        if($mutationExit -ne 1 -or !($witness -match 'FAIL:')) { throw "Mutant did not fail through an assertion: $($mutant.Name), exit=$mutationExit" }
        Write-Output ('DISCRIMINATED: '+$mutant.Name+' (expected assertion exit 1; harness only)')
    }
    Write-Output 'PASS: seven GCC x86 groups and three rebuilt negative controls; no native/game acceptance.'
} finally { $env:PATH=$previousPath }
