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
Import-Module (Join-Path $repo 'scripts/NativeTestProcess.psm1') -Force
$identity=$null
try {
    $start=[DateTime]::UtcNow
    # Harmless non-game child: output inherited by a blocking cmd helper used to
    # keep the previous supervisor from returning. There is no shell here.
    $identity=Start-NativeTestProcess -Exe $Python -WorkingDirectory $repo -Arguments '-c "import time; print(123,flush=True); time.sleep(4)"'
    $ms=([DateTime]::UtcNow-$start).TotalMilliseconds
    if($ms -gt 2500 -or !(Get-OwnedTestProcess $identity)) { throw 'Launch was not nonblocking' }
    if($identity.Affinity -le 0 -or $identity.Affinity -gt 65535) { throw 'Affinity not limited before execution' }
    $wrong=[pscustomobject]@{Id=$identity.Id;Path=$identity.Path;StartTicks=($identity.StartTicks+1)}
    $refused=$false;try { Get-OwnedTestProcess $wrong|Out-Null } catch { $refused=$_.Exception.Message -like '*PID identity changed*' }
    if(!$refused -or !(Get-OwnedTestProcess $identity)) { throw 'PID reuse control failed' }
    (Get-OwnedTestProcess $identity).WaitForExit(6000)|Out-Null
    if(Get-OwnedTestProcess $identity) { throw 'Benign child did not finish' }
    Write-Output ("PASS: parse, plan-only, stale directory, nonblocking native child (${ms}ms), affinity and PID-reuse refusal; no game launch")
} finally {
    $p=Get-OwnedTestProcess $identity
    if($p) { $p.Kill();$p.WaitForExit(8000)|Out-Null }
}
