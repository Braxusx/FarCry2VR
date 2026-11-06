param(
    [Parameter(Mandatory)][string]$Trial,
    [Parameter(Mandatory)][string]$Probe,
    [switch]$CloseAfterLoad
)
# Physical test launcher: real head/controllers, automatic Story Mode/Continue.
# No synthetic pose fixture is set. Only the process launched here is owned.
$ErrorActionPreference='Stop'
$Trial=(Resolve-Path -LiteralPath $Trial).Path
$Probe=(Resolve-Path -LiteralPath $Probe).Path
$manifest=Get-Content "$Trial/test-manifest.json" -Raw|ConvertFrom-Json
foreach($binary in $manifest.original_binaries){if((Get-FileHash -LiteralPath $binary.path).Hash -ne $binary.sha256){throw 'Original binary changed'}}
foreach($file in $manifest.files){
    if($file.name -ne 'fc2vr.ini' -and (Get-FileHash -LiteralPath (Join-Path $manifest.bin $file.name)).Hash -ne $file.sha256){throw "Trial binary changed: $($file.name)"}
}
if($env:FC2VR_BODY_YAW_TEST_FILE){throw 'Synthetic pose fixture is set; refusing physical launch'}
$output=Join-Path $Trial ('load-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
[void](New-Item -ItemType Directory $output)
$gamePid=0;$gameProcess=$null;$loaded=$false
try{
    & "$Trial/Launch-Test.ps1"
    $launch=Get-Content "$Trial/launch.json" -Raw|ConvertFrom-Json
    $gamePid=$launch.pid;$started=([DateTime]$launch.started).ToUniversalTime()
    $gameProcess=Get-Process -Id $gamePid -ErrorAction Stop
    [void]$gameProcess.Handle
    $log=Join-Path $manifest.bin 'fc2vr.log';$deadline=[DateTime]::UtcNow.AddSeconds(240)
    $confirms=0;$lastConfirm=[DateTime]::MinValue
    while([DateTime]::UtcNow -lt $deadline){
        if(!(Get-Process -Id $gamePid -ErrorAction SilentlyContinue)){throw 'Owned game exited before loading'}
        $fresh=(Get-Item -LiteralPath $log -ErrorAction SilentlyContinue)
        if($fresh -and $fresh.LastWriteTimeUtc -ge $started){
            $text=Get-Content -LiteralPath $log -Raw
            if($text -match 'native-save-ready: on-foot player and unposed camera aligned'){$loaded=$true;break}
            if($text -match 'frame \d+: xr running=' -and $confirms -lt 5 -and ([DateTime]::UtcNow-$lastConfirm).TotalSeconds -ge 10){
                $focus=Start-Process $Probe -WindowStyle Hidden -Wait -PassThru -ArgumentList '--inspect-game-focus' -RedirectStandardOutput "$output/focus.json" -RedirectStandardError "$output/focus-errors.log"
                if(!$focus.ExitCode){
                    $identity=Get-Content "$output/focus.json" -Raw|ConvertFrom-Json
                    if($identity.pid -ne $gamePid){throw 'Input probe selected a different game process'}
                    $key=Start-Process $Probe -WindowStyle Hidden -Wait -PassThru -ArgumentList '--menu-key','confirm' -RedirectStandardOutput "$output/key.log" -RedirectStandardError "$output/key-errors.log"
                    if(!$key.ExitCode){$confirms++;$lastConfirm=[DateTime]::UtcNow;"Sent confirm $confirms to owned PID $gamePid"|Add-Content "$output/input.log"}
                }
            }
        }
        Start-Sleep -Seconds 2
    }
    if(!$loaded){throw 'Could not verify automatic save loading; see the input and native logs'}
    # Camera alignment is native evidence. Allow remaining loading overlays to
    # finish without sending further input or assuming another Enter is safe.
    Start-Sleep -Seconds 10
    $capture=Start-Process $Probe -WindowStyle Hidden -Wait -PassThru -ArgumentList '--capture-window',('"'+"$output/gameplay.png"+'"') -RedirectStandardOutput "$output/capture.log" -RedirectStandardError "$output/capture-errors.log"
    @{pid=$gamePid;loaded=$true;confirms=$confirms;evidence='native on-foot player and unposed camera aligned';time=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content "$output/result.json"
    Copy-Item -LiteralPath $log -Destination "$output/native.log"
    "Latest save loaded automatically; native readiness verified (PID $gamePid)."
}finally{
    if($gamePid -and (Test-Path -LiteralPath $log)){Copy-Item -LiteralPath $log -Destination "$output/native.log" -ErrorAction SilentlyContinue}
    if($CloseAfterLoad -and $gameProcess -and !$gameProcess.HasExited){$gameProcess.Kill();[void]$gameProcess.WaitForExit(5000)}
}
