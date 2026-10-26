param(
    [switch]$Run,
    [switch]$StartupOnly,
    [switch]$MenuDiagnostics,
    [switch]$ProductionProxy,
    [ValidateRange(0,120)][int]$MenuQuitGraceSeconds=0,
    [string]$RunRoot,
    [string]$GameBin,
    [string]$DebugDll,
    [string]$ExpectedDllSha256,
    [string]$Profile,
    [string]$SaveDir,
    [string]$DeclaredSave,
    [string]$TestIni,
    [string]$TestProfile,
    [string]$Headset,
    [string]$RuntimeManifest,
    [string]$RuntimeDll,
    [string]$SimTools='D:/Dev Debug/xr-sim/tools',
    [ValidateRange(30,300)][int]$MaxSeconds=240
)
$ErrorActionPreference='Stop'
if(!$Run) { Write-Output 'PLAN ONLY: one suspended/affinity-constrained launch; fresh PID-acknowledged xr-sim; bounded watchdog and hash restoration. Supply explicit paths and -Run.'; return }
if($MenuQuitGraceSeconds -and !$StartupOnly) {throw 'Menu quit grace is only supported for startup observation'}
foreach($name in @('RunRoot','GameBin','DebugDll','ExpectedDllSha256','Profile','SaveDir','DeclaredSave','TestIni','TestProfile','Headset','RuntimeManifest','RuntimeDll')) {
    if(! (Get-Variable -Name $name -ValueOnly)) { throw "Missing -$name" }
}
if(Test-Path -LiteralPath $RunRoot) { throw 'RunRoot must not exist: refusing stale state or backup overwrite' }
foreach($name in @('GameBin','DebugDll','Profile','SaveDir','DeclaredSave','TestIni','TestProfile','Headset','RuntimeManifest','RuntimeDll','SimTools')) {
    Set-Variable -Name $name -Value (Resolve-Path -LiteralPath (Get-Variable -Name $name -ValueOnly)).ProviderPath
}
$gameExe=Join-Path $GameBin 'FarCry2.exe'
function Hash($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Json($name,$value) { [IO.File]::WriteAllText((Join-Path $RunRoot $name),($value|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false)) }
function AssertNoGame {
    foreach($candidate in @(Get-Process -Name FarCry2 -ErrorAction SilentlyContinue)) {
        try { if(!$candidate.HasExited) { throw 'Existing FarCry2: refusing launch or shared-file changes' } }
        finally { $candidate.Dispose() }
    }
}
AssertNoGame
if((Hash $gameExe) -ne 'b5e081fe22c09618eeee68e0e470ce29c7e539100faf849c926b77a4e2fb4db8' -or
    (Hash (Join-Path $GameBin 'Dunia.dll')) -ne '6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765') { throw 'Unknown target image' }
if($ExpectedDllSha256 -notmatch '^[a-f0-9]{64}$' -or (Hash $DebugDll) -ne $ExpectedDllSha256) { throw 'Debug artifact identity mismatch' }
$payload=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($DebugDll))
if($ProductionProxy) {
    if(!$StartupOnly -or $MenuDiagnostics -or !$payload.Contains('menu-record-guard-2') -or $payload.Contains('crash-context-1')) {throw 'Production proxy requires startup-only, no diagnostic opt-in and verified production payload'}
} elseif(!$payload.Contains('crash-context-1') -or !$payload.Contains('startup-camera-1')) { throw 'Required debug build markers missing' }
$manifest=Get-Content -LiteralPath $RuntimeManifest -Raw|ConvertFrom-Json
$manifestLibrary=$manifest.runtime.library_path
if(![IO.Path]::IsPathRooted($manifestLibrary)) { $manifestLibrary=Join-Path (Split-Path -Parent $RuntimeManifest) $manifestLibrary }
$manifestLibrary=[IO.Path]::GetFullPath($manifestLibrary)
if($manifestLibrary -ne $RuntimeDll) { throw 'Manifest runtime DLL mismatch' }
$bytes=[IO.File]::ReadAllBytes($RuntimeDll);$nt=[BitConverter]::ToInt32($bytes,60)
if([BitConverter]::ToUInt16($bytes,$nt+4) -ne 0x14c) { throw 'Expected x86 runtime' }
Import-Module (Join-Path $PSScriptRoot 'NativeTestProcess.psm1') -Force
$null=New-Item -ItemType Directory -Path $RunRoot
foreach($name in @('backup','sim','scenarios')) { $null=New-Item -ItemType Directory -Path (Join-Path $RunRoot $name) }
Copy-Item -LiteralPath $Headset -Destination (Join-Path $RunRoot 'sim/headset.json')
$owned=$null;$attempted=$false;$failure='';$forced=$false;$restored=$false;$terminal=$false;$bootstrapApplied=$false;$startupSurvived=$false
$closeRequestedUtc=$null;$closeMethod='none';$finalLogFailure=''
$backups=@();$saveBackups=@();$savedEnvironment=@{}
try {
    foreach($item in @(@{Path=(Join-Path $GameBin 'dinput8.dll');Name='dinput8.dll'},@{Path=(Join-Path $GameBin 'fc2vr.ini');Name='fc2vr.ini'},@{Path=$Profile;Name='GamerProfile.xml'},@{Path=(Join-Path $GameBin 'fc2vr.log');Name='fc2vr.log'},@{Path=(Join-Path $GameBin 'fc2vr_prev.log');Name='fc2vr_prev.log'})) {
        $entry=@{Path=$item.Path;Backup=(Join-Path $RunRoot ('backup/'+$item.Name));Present=(Test-Path -LiteralPath $item.Path -PathType Leaf);Sha256=''}
        if($entry.Present) { $entry.Sha256=Hash $entry.Path;Copy-Item -LiteralPath $entry.Path -Destination $entry.Backup }
        $backups+=$entry
    }
    foreach($save in Get-ChildItem -LiteralPath $SaveDir -File) {
        $entry=@{Path=$save.FullName;Backup=(Join-Path $RunRoot ('backup/save-'+$save.Name));Sha256=(Hash $save.FullName)}
        Copy-Item -LiteralPath $entry.Path -Destination $entry.Backup;$saveBackups+=$entry
    }
    Copy-Item -LiteralPath $DeclaredSave -Destination (Join-Path $RunRoot 'declared-save.sav')
    Json 'before.json' @{Files=$backups;Saves=$saveBackups;RuntimeManifest=$RuntimeManifest;RuntimeSha=(Hash $RuntimeDll);ExpectedDll=$ExpectedDllSha256;Scenario=if($StartupOnly){'none-startup-observation'}else{'arm-sweep'};LaunchLimit=1;MaxSeconds=$MaxSeconds}
    Copy-Item -LiteralPath $DebugDll -Destination (Join-Path $GameBin 'dinput8.dll') -Force
    Copy-Item -LiteralPath $TestIni -Destination (Join-Path $GameBin 'fc2vr.ini') -Force
    Copy-Item -LiteralPath $TestProfile -Destination $Profile -Force
    # The previous logs must not remain visible to the supervisor while the new
    # process is still in the loader. Move (do not discard) their backed-up bytes
    # into this private run; finally restores the exact originals.
    foreach($name in @('fc2vr.log','fc2vr_prev.log')) {
        $prior=Join-Path $GameBin $name
        if(Test-Path -LiteralPath $prior) { Move-Item -LiteralPath $prior -Destination (Join-Path $RunRoot ('prior-'+$name)) }
    }
    $environment=@{XR_RUNTIME_JSON=$RuntimeManifest;XRSIM_DIR=(Join-Path $RunRoot 'sim');XRSIM_HEADSET_CONFIG=(Join-Path $RunRoot 'sim/headset.json');XR_API_LAYER_PATH=$null;XR_ENABLE_API_LAYERS=$null;XRTAPE_DIR=$null;FC2VR_SCENARIO='arm-sweep';FC2VR_SCENARIO_ARM='ALLOW_SYNTHETIC_INPUT';FC2VR_SCENARIO_OUT=(Join-Path $RunRoot 'scenarios');FC2VR_SCENARIO_FIXTURE='native-startup-camera-1';FC2VR_SCENARIO_SAVE=(Join-Path $RunRoot 'declared-save.sav');FC2VR_SCENARIO_PROFILE=$Profile}
    $environment.FC2VR_MENU_DIAG=if($MenuDiagnostics){'1'}else{$null}
    if($StartupOnly) { foreach($key in @('FC2VR_SCENARIO','FC2VR_SCENARIO_ARM','FC2VR_SCENARIO_FIXTURE','FC2VR_SCENARIO_SAVE','FC2VR_SCENARIO_PROFILE')) {$environment[$key]=$null} }
    foreach($key in $environment.Keys) { $savedEnvironment[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$environment[$key],'Process') }
    AssertNoGame
    $launchAt=[DateTime]::UtcNow;$deadline=$launchAt.AddSeconds($MaxSeconds)
    $attempted=$true # no retry, including CreateProcess/setup/early-exit failures
    $owned=Start-NativeTestProcess -Exe $gameExe -WorkingDirectory $GameBin
    Json 'owned.json' $owned
    Write-Output ("OWNED pid="+$owned.Id+" affinity="+$owned.Affinity+" watchdog="+$MaxSeconds+"s; exactly one launch")
    while([DateTime]::UtcNow -lt $deadline) {
        if(!(Test-OwnedTestProcess $owned)) { throw 'Owned game exited before scenario completion' }
        if(Test-Path -LiteralPath (Join-Path $RunRoot 'stop-test.txt')) { throw 'Operator stop before terminal' }
        $stateFile=Join-Path $RunRoot 'sim/state.json'
        if(!$bootstrapApplied -and (Test-Path -LiteralPath $stateFile)) {
            $state=$null;try { $state=Get-Content -LiteralPath $stateFile -Raw|ConvertFrom-Json } catch {}
            if($state) {
                if($state.pid -ne $owned.Id -or $state.runtime -ne 'xr-sim' -or $state.graphics -ne 'D3D11') { throw 'Wrong/foreign simulator state' }
                $bootstrap=@('focus policy permissive','state focused','profile touch','head pose 0 1.7 0 0 0 0','hand l follow off','hand r follow off','hand l grip pose -0.2 1.45 -0.45 0 0 0','hand l aim pose -0.2 1.45 -0.48 0 0 0','hand r grip pose 0.2 1.45 -0.45 0 0 0','hand r aim pose 0.2 1.45 -0.48 0 0 0','input clear')
                $ack=& (Join-Path $SimTools 'xrsim-cmd.ps1') -Dir (Join-Path $RunRoot 'sim') -TimeoutSec 5 -Quiet -Lines $bootstrap
                if($ack.pid -ne $owned.Id -or $ack.cmdSeq -ne ($state.cmdSeq+$bootstrap.Count) -or $ack.lastCmd -ne $bootstrap[-1] -or $ack.lastCmdError) { throw 'Bootstrap acknowledgement mismatch' }
                Json 'bootstrap-ack.json' $ack;$bootstrapApplied=$true
                Write-Output 'BOOTSTRAP ACK: fresh matching PID/sequence, fixed hands and focus'
            }
        }
        $log=Join-Path $GameBin 'fc2vr.log'
        if(Test-Path -LiteralPath $log) {
            $text=Read-NativeTestText -Path $log
            $logInfo=Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
            if($null -ne $text -and $logInfo) {
                $logFailure=Get-NativeTestLogFailure -Text $text -Fresh ($logInfo.LastWriteTimeUtc -ge $launchAt) -ScenarioTesting ([int](!$ProductionProxy))
                if($logFailure) { throw $logFailure }
            }
        }
        $traces=@(Get-ChildItem -LiteralPath (Join-Path $RunRoot 'scenarios') -Filter trace.jsonl -Recurse -File)
        if($traces.Count -gt 1) { throw 'Multiple traces in a one-launch fixture' }
        if($traces.Count -eq 1 -and (Read-NativeTestText -Path $traces[0].FullName) -match '"event":"end"') {
            $terminal=$true;Write-Output ('SCENARIO TERMINAL '+$traces[0].FullName);Start-Sleep -Seconds 3;break
        }
        Start-Sleep -Milliseconds 250
    }
    if($StartupOnly -and [DateTime]::UtcNow -ge $deadline -and (Test-OwnedTestProcess $owned)) {
        $startupSurvived=$true;Write-Output 'STARTUP SURVIVAL WINDOW COMPLETE (not menu/gameplay/HMD acceptance)'
    }
    if(!$terminal -and !$startupSurvived) { throw 'Overall test watchdog (no scenario terminal)' }
    if(!$StartupOnly -and !$bootstrapApplied) { throw 'No bootstrap acknowledgement' }
    if($startupSurvived -and $MenuQuitGraceSeconds) {
        # An external, state-observing UI driver can use the native Quit menu.
        # Never infer a clean quit from process exit code or the survival flag.
        $closeMethod='external-after-window';$closeRequestedUtc=[DateTime]::UtcNow.ToString('o')
        Write-Output ("MENU QUIT GRACE: "+$MenuQuitGraceSeconds+' seconds after completed startup window')
        $quitDeadline=[DateTime]::UtcNow.AddSeconds($MenuQuitGraceSeconds)
        while((Test-OwnedTestProcess $owned) -and [DateTime]::UtcNow -lt $quitDeadline) {Start-Sleep -Milliseconds 250}
    }
} catch { $failure=$_.Exception.Message;Write-Output ('TEST FAILURE: '+$failure) }
finally {
    try {
        $p=Get-OwnedTestProcess $owned
        if($p) {
            $closeMethod='window-close';$closeRequestedUtc=[DateTime]::UtcNow.ToString('o')
            try {$null=$p.CloseMainWindow()} finally {$p.Dispose();$p=$null}
            $closeUntil=[DateTime]::UtcNow.AddSeconds(12)
            while((Test-OwnedTestProcess $owned) -and [DateTime]::UtcNow -lt $closeUntil) { Start-Sleep -Milliseconds 250 }
            $p=Get-OwnedTestProcess $owned
            if($p) { try {$forced=$true;$p.Kill();$p.WaitForExit(8000)|Out-Null} finally {$p.Dispose();$p=$null} }
        }
        if(Test-OwnedTestProcess $owned) { throw 'Owned game did not stop; restoration unsafe' }
        AssertNoGame
        foreach($name in @('fc2vr.log','fc2vr_prev.log')) {
            $path=Join-Path $GameBin $name
            if(Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $RunRoot ('test-'+$name)) }
        }
        $retainedLog=Join-Path $RunRoot 'test-fc2vr.log'
        if(Test-Path -LiteralPath $retainedLog) {
            $finalLogFailure=Get-NativeTestLogFailure -Text (Read-NativeTestText -Path $retainedLog) -Fresh $true -ScenarioTesting ([int](!$ProductionProxy))
            if($finalLogFailure -and !$failure.Contains($finalLogFailure)) {$failure+='; final log: '+$finalLogFailure}
        }
        foreach($entry in $backups) {
            if($entry.Present) { Copy-NativeTestFile -Source $entry.Backup -Destination $entry.Path;if((Hash $entry.Path) -ne $entry.Sha256) { throw 'File restoration hash mismatch' } }
            elseif(Test-Path -LiteralPath $entry.Path) { Remove-Item -LiteralPath $entry.Path -Force }
        }
        foreach($entry in $saveBackups) {
            if(!(Test-Path -LiteralPath $entry.Path) -or (Hash $entry.Path) -ne $entry.Sha256) { Copy-NativeTestFile -Source $entry.Backup -Destination $entry.Path }
            if((Hash $entry.Path) -ne $entry.Sha256) { throw 'Save restoration hash mismatch' }
        }
        $newSaves=@(Get-ChildItem -LiteralPath $SaveDir -File | Where-Object { $_.FullName -notin @($saveBackups|ForEach-Object {$_.Path}) })
        if($newSaves.Count) { throw 'Unexpected new saves retained for inspection; not claiming exact save inventory restoration' }
        $restored=$true
    } catch { $failure+='; cleanup: '+$_.Exception.Message;Write-Output ('CLEANUP FAILURE: '+$_) }
    foreach($key in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key,$savedEnvironment[$key],'Process') }
    $stopped=$null
    if($owned) { $stopped=!(Test-OwnedTestProcess $owned) }
    elseif(!$attempted) { $stopped=$true }
    # A lost identity cannot prove a created process stopped. Preserve unknown,
    # rather than reporting !null as a false successful cleanup receipt.
    Json 'cleanup.json' @{LaunchAttempts=[int]$attempted;Owned=$owned;ProcessStopped=$stopped;Forced=$forced;Restored=$restored;Terminal=$terminal;StartupOnly=[bool]$StartupOnly;StartupSurvived=$startupSurvived;BootstrapAcknowledged=$bootstrapApplied;CloseMethod=$closeMethod;CloseRequestedUtc=$closeRequestedUtc;FinalLogFailure=$finalLogFailure;Failure=$failure;FinishedUtc=[DateTime]::UtcNow.ToString('o')}
    Write-Output ("FINISHED restored=$restored forced=$forced failure=$failure")
}
if($failure -or !$restored -or $forced) { exit 1 }
# A terminal marker only requests checking; run verify_scenario_run.py separately.
