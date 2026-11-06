param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path $Output){throw 'Choose fresh output directory'}
$out=(New-Item -ItemType Directory $Output).FullName
function Phase([int]$id,[bool]$freeze=$false){
    [IO.File]::WriteAllText("$out/control.new",(@{id=$id;yaw=90;pitch=0;roll=0;tracked=1;allowed=1;reset=0;freeze=$freeze}|ConvertTo-Json -Compress))
    if(Test-Path "$out/control.json"){[IO.File]::Replace("$out/control.new","$out/control.json","$out/control.previous")}else{[IO.File]::Move("$out/control.new","$out/control.json")}
}
Phase 1
$writer=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-File',('"'+"$PSScriptRoot/body_yaw_writer.ps1"+'"'),'-GamePid',$PID,'-PoseFile',('"'+"$out/pose.csv"+'"'),'-ControlFile',('"'+"$out/control.json"+'"')) -RedirectStandardError "$out/errors.log"
try{
    Start-Sleep -Seconds 2
    foreach($id in 2..40){Phase $id;Start-Sleep -Milliseconds 40;if($writer.HasExited){throw 'Writer exited during atomic phase changes'}}
    Start-Sleep -Milliseconds 250
    $last=[IO.File]::ReadAllText("$out/pose.csv");if($last -notmatch "^$PID,40\d{5},90,0,0,1,1,0$"){throw "Incorrect final phase: $last"}
    Phase 41 $true;Start-Sleep -Milliseconds 200
    $frozen=[IO.File]::ReadAllText("$out/pose.csv");Start-Sleep -Milliseconds 250
    if($frozen -ne [IO.File]::ReadAllText("$out/pose.csv")){throw 'Frozen source advanced'}
    Phase 42;Start-Sleep -Milliseconds 200
    if([IO.File]::ReadAllText("$out/pose.csv") -notmatch "^$PID,42\d{5},"){throw 'Writer failed to recover'}
    $lock=[IO.File]::Open("$out/control.json",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    try{Start-Sleep -Milliseconds 300;if($writer.HasExited){throw 'Writer died on transient sharing lock'}}finally{$lock.Dispose()}
    Phase 43;Start-Sleep -Milliseconds 200
    if([IO.File]::ReadAllText("$out/pose.csv") -notmatch "^$PID,43\d{5},"){throw 'Writer did not recover from sharing lock'}
    if((Get-Item "$out/errors.log").Length){throw 'Writer reported errors'}
    'PASS: 40 atomic phase changes, frozen source and sharing-lock recovery'|Set-Content "$out/result.txt"
    Get-Content "$out/result.txt"
}finally{if(!$writer.HasExited){Stop-Process -Id $writer.Id -Force}}
