param([string]$Python='C:/Users/meise/AppData/Local/Programs/Python/Python312/python.exe')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$script=Join-Path $repo 'scripts/Test-NativeScenario.ps1'
foreach($path in @($script,(Join-Path $repo 'scripts/NativeTestProcess.psm1'))) {
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw ($errors|Out-String) }
}
$plan=& $script
if($plan -notmatch '^PLAN ONLY') { throw 'Default invocation must be non-mutating plan only' }
# Used directory is refused before file resolution, backup, deployment or launch.
$args=@{Run=$true;RunRoot=$repo;GameBin='missing';DebugDll='missing';ExpectedDllSha256='missing';Profile='missing';SaveDir='missing';DeclaredSave='missing';TestIni='missing';TestProfile='missing';Headset='missing';RuntimeManifest='missing';RuntimeDll='missing'}
$refused=$false
try { & $script @args } catch { $refused=$_.Exception.Message -like '*RunRoot must not exist*' }
if(!$refused) { throw 'Used-directory guard failed' }
$launcherSource=[IO.File]::ReadAllText($script)
if($launcherSource.Contains('[IO.File]::ReadAllText($log)') -or $launcherSource.Contains('[IO.File]::ReadAllText($traces')) {throw 'Exclusive live telemetry reader was reintroduced'}
if(!$launcherSource.Contains('Read-NativeTestText -Path $log') -or !$launcherSource.Contains('Read-NativeTestText -Path $traces[0].FullName')) {throw 'Shared reader is not wired to both live telemetry files'}
if(!$launcherSource.Contains('Get-NativeTestLogFailure -Text (Read-NativeTestText -Path $retainedLog)') -or !$launcherSource.Contains('FinalLogFailure=$finalLogFailure')) {throw 'Post-close native failure check/receipt missing'}
Import-Module (Join-Path $repo 'scripts/NativeTestProcess.psm1') -Force
# Reproduces the actual run-02 race without launching a game: old installed
# sessions can have both a different marker and an old native exception.
$stale='FC2VR loaded (1.0.1); XR: runtime VirtualDesktopXR; EXCEPTION c0000005'
if(Get-NativeTestLogFailure -Text $stale -Fresh $false) { throw 'Stale native log was consumed' }
if((Get-NativeTestLogFailure -Text $stale -Fresh $true) -ne 'Wrong loaded mod marker') { throw 'Wrong fresh marker was accepted' }
$current='FC2VR loaded (contribution startup-camera-1/scenario-runner-2, scenarioTesting=1); XR: runtime xr-sim 1.0.0'
if(Get-NativeTestLogFailure -Text $current -Fresh $true) { throw 'Fresh matching marker was rejected' }
$production=$current.Replace('scenarioTesting=1','scenarioTesting=0')
if(Get-NativeTestLogFailure -Text $production -Fresh $true -ScenarioTesting 0) {throw 'Production marker was rejected'}
if((Get-NativeTestLogFailure -Text $production -Fresh $true) -ne 'Wrong loaded mod marker') {throw 'Production marker accepted as debug'}
if((Get-NativeTestLogFailure -Text $current -Fresh $true -ScenarioTesting 0) -ne 'Wrong loaded mod marker') {throw 'Debug marker accepted as production'}
if((Get-NativeTestLogFailure -Text ($current+' EXCEPTION c0000005') -Fresh $true) -notlike 'Native access violation*') { throw 'Fresh AV was accepted' }
if((Get-NativeTestLogFailure -Text ($current.Replace('xr-sim','wrong-runtime')) -Fresh $true) -notlike 'Wrong runtime*') { throw 'Fresh wrong runtime was accepted' }
$fixtureRoot=Join-Path ([IO.Path]::GetTempPath()) ('fc2vr-launcher-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixtureRoot
$fixtureLog=Join-Path $fixtureRoot 'active.log'
$fixtureCopy=Join-Path $fixtureRoot 'copy.log'
$writer=$null
try {
    # Exact sharing discriminator: a live native writer that permits readers.
    $writer=[IO.File]::Open($fixtureLog,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $bytes=[Text.Encoding]::UTF8.GetBytes($current);$writer.Write($bytes,0,$bytes.Length);$writer.Flush()
    $legacyFailed=$false
    try { [IO.File]::ReadAllText($fixtureLog)|Out-Null } catch { $legacyFailed=$true }
    if(!$legacyFailed) { throw 'Sharing fixture did not expose legacy reader failure' }
    if((Read-NativeTestText $fixtureLog) -ne $current) { throw 'Shared live-writer read failed' }
    $extra=[Text.Encoding]::UTF8.GetBytes(' EXCEPTION c0000005');$writer.Write($extra,0,$extra.Length);$writer.Flush()
    if((Get-NativeTestLogFailure -Text (Read-NativeTestText $fixtureLog) -Fresh $true) -notlike 'Native access violation*') { throw 'Appended crash was missed while writer remained open' }
    $bounded=$false;try { Read-NativeTestText -Path $fixtureLog -MaxBytes 1|Out-Null } catch { $bounded=$_.Exception.Message -like '*bounded read limit*' }
    if(!$bounded) { throw 'Oversize telemetry was not refused' }
    $writer.Dispose();$writer=$null
    $writer=[IO.File]::Open($fixtureLog,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None)
    if($null -ne (Read-NativeTestText $fixtureLog)) { throw 'Exclusive lock should be pending' }
    $writer.Dispose();$writer=$null
    if($null -ne (Read-NativeTestText (Join-Path $fixtureRoot 'not-yet-created.log'))) { throw 'Absent log should be pending' }
    Copy-NativeTestFile -Source $fixtureLog -Destination $fixtureCopy
    if((Get-FileHash -LiteralPath $fixtureLog).Hash -ne (Get-FileHash -LiteralPath $fixtureCopy).Hash) {throw 'Restoration copy changed bytes'}
} finally {
    if($writer) {$writer.Dispose()}
    # Exact test-owned files only, never a recursive computed-directory delete.
    if(Test-Path -LiteralPath $fixtureLog) {Remove-Item -LiteralPath $fixtureLog}
    if(Test-Path -LiteralPath $fixtureCopy) {Remove-Item -LiteralPath $fixtureCopy}
    Remove-Item -LiteralPath $fixtureRoot
}
$identity=$null
try {
    $start=[DateTime]::UtcNow
    # Harmless non-game child: output inherited by a blocking cmd helper used to
    # keep the previous supervisor from returning. There is no shell here.
    $identity=Start-NativeTestProcess -Exe $Python -WorkingDirectory $repo -Arguments '-c "import time; print(123,flush=True); time.sleep(4)"'
    $ms=([DateTime]::UtcNow-$start).TotalMilliseconds
    if($ms -gt 2500 -or !(Get-OwnedTestProcess $identity)) { throw 'Launch was not nonblocking' }
    if($identity.Affinity -le 0 -or $identity.Affinity -gt 65535) { throw 'Affinity not limited before execution' }
    $hostProbe=Get-Process -Id $PID
    try {
        $hostProbe.Refresh();$handlesBefore=$hostProbe.HandleCount
        for($poll=0;$poll -lt 40;++$poll) {if(!(Test-OwnedTestProcess $identity)){throw 'Polling lost live child'}}
        $hostProbe.Refresh();$handlesAfter=$hostProbe.HandleCount
        if($handlesAfter-$handlesBefore -gt 12) {throw 'Ownership polling retained process handles'}
    } finally {$hostProbe.Dispose()}
    $wrong=[pscustomobject]@{Id=$identity.Id;Path=$identity.Path;StartTicks=($identity.StartTicks+1)}
    $refused=$false;try { Get-OwnedTestProcess $wrong|Out-Null } catch { $refused=$_.Exception.Message -like '*PID identity changed*' }
    if(!$refused -or !(Get-OwnedTestProcess $identity)) { throw 'PID reuse control failed' }
    $wrongPath=[pscustomobject]@{Id=$identity.Id;Path=($identity.Path+'.wrong');StartTicks=$identity.StartTicks}
    $refused=$false;try { Get-OwnedTestProcess $wrongPath|Out-Null } catch { $refused=$_.Exception.Message -like '*PID identity changed*' }
    if(!$refused -or !(Get-OwnedTestProcess $identity)) { throw 'Wrong-path control failed' }
    (Get-OwnedTestProcess $identity).WaitForExit(6000)|Out-Null
    if(Get-OwnedTestProcess $identity) { throw 'Benign child did not finish' }
    # An already-exited child still returns its pre-resume kernel identity; the
    # old post-resume MainModule lookup could throw and lose the owned PID.
    for($trial=0;$trial -lt 8;++$trial) {
        $quick=Start-NativeTestProcess -Exe $Python -WorkingDirectory $repo -Arguments '-c "pass"'
        if(!$quick.Id -or !$quick.Path -or !$quick.StartTicks) { throw 'Short-lived child identity lost' }
        $qp=Get-OwnedTestProcess $quick
        if($qp) {$qp.WaitForExit(4000)|Out-Null}
        if(Get-OwnedTestProcess $quick) {throw 'Short-lived child remains'}
    }
    Write-Output ("PASS: parse, plan-only, stale directory/log, live-writer legacy-failure/shared-read, appended AV, exclusive/missing/oversize, wrong-marker/runtime, pre-resume/short-lived identity, nonblocking native child (${ms}ms), affinity and path/PID-reuse refusal; no game launch")
} finally {
    $p=Get-OwnedTestProcess $identity
    if($p) { $p.Kill();$p.WaitForExit(8000)|Out-Null }
}
