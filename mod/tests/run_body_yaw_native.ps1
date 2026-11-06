param(
    [Parameter(Mandatory)][string]$FixtureDll,
    [Parameter(Mandatory)][string]$Probe,
    [string]$LaunchTemplate=(Join-Path $PSScriptRoot 'launch_native_game.ps1'),
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet(0,1)][int]$Enabled=1,
    [switch]$GrassFix,
    [switch]$GrassMotion,
    [switch]$BodyOnly,
    [switch]$LeftWeapon,
    [switch]$WeaponHandToggle
)
# Owned, bounded native test. Supply the existing source-built trial manifest.
# Does not patch original game binaries or send save commands. Restores DLL/settings.
$ErrorActionPreference='Stop'
if(Get-Process FarCry2,FarCry2VR -ErrorAction SilentlyContinue){throw 'Game already running'}
$trial=Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
foreach($binary in $trial.original_binaries){if((Get-FileHash -LiteralPath $binary.path).Hash -ne $binary.sha256){throw "Original binary changed: $($binary.path)"}}
if(Test-Path -LiteralPath $Output){throw 'Choose a fresh output directory'}
$out=New-Item -ItemType Directory -Path $Output
$out=$out.FullName
$dll=Join-Path $trial.bin 'dinput8.dll';$ini=Join-Path $trial.bin 'fc2vr.ini';$log=Join-Path $trial.bin 'fc2vr.log'
Copy-Item -LiteralPath $dll -Destination "$out/before-dinput8.dll"
Copy-Item -LiteralPath $ini -Destination "$out/before-fc2vr.ini"
Copy-Item -LiteralPath $trial.profile -Destination "$out/before-profile.xml"
Copy-Item -LiteralPath $log -Destination "$out/before-fc2vr.log" -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $Manifest -Destination "$out/test-manifest.json"
Copy-Item -LiteralPath $LaunchTemplate -Destination "$out/Launch-Test.ps1"
$gamePid=0;$gameProcess=$null;$writer=$null;$oldFixture=$env:FC2VR_BODY_YAW_TEST_FILE
function Set-Phase($id,$name,$yaw=0,$pitch=0,$roll=0,$tracked=1,$allowed=1,$reset=0,$freeze=$false){
    if($writer -and $writer.HasExited){throw 'Pose writer exited; see writer-errors.log'}
    $phase=@{id=$id;name=$name;yaw=$yaw;pitch=$pitch;roll=$roll;tracked=$tracked;allowed=$allowed;reset=$reset;freeze=$freeze}
    $json=$phase|ConvertTo-Json -Compress
    [IO.File]::WriteAllText("$out/control.new",$json)
    if(Test-Path "$out/control.json"){[IO.File]::Replace("$out/control.new","$out/control.json","$out/control.previous")}else{[IO.File]::Move("$out/control.new","$out/control.json")}
    $json|Add-Content "$out/phases.jsonl"
}
try{
    Copy-Item -LiteralPath $FixtureDll -Destination $dll
    $config=[IO.File]::ReadAllText($ini)
    $config=[regex]::Replace($config,'(?m)^HeadsetBodyYaw=.*\r?\n?','')
    $config=$config.Replace('[VR]',"[VR]`r`nHeadsetBodyYaw=$Enabled")
    $config=[regex]::Replace($config,'(?m)^LeftHandedWeapon=.*\r?\n?','')
    $config=$config.Replace('[VR]',("[VR]`r`nLeftHandedWeapon="+[int][bool]$LeftWeapon))
    if($BodyOnly){
        foreach($key in @('WeaponInHand','AimFromController','DumpSkeleton')){
            $config=[regex]::Replace($config,('(?m)^'+$key+'=.*\r?\n?'),'')
            $config=$config.Replace('[VR]',("[VR]`r`n"+$key+'=0'))
        }
    }
    $config=[regex]::Replace($config,'(?m)^GrassHeadYawFix=.*\r?\n?','')
    $config=$config.Replace('[VR]',("[VR]`r`nGrassHeadYawFix="+[int][bool]$GrassFix))
    $config=[regex]::Replace($config,'(?m)^BodyYawDiagnostics=.*\r?\n?','')
    $config=$config.Replace('[VR]',"[VR]`r`nBodyYawDiagnostics=1")
    $config=[regex]::Replace($config,'(?m)^RenderScale=.*','RenderScale=0.5')
    [IO.File]::WriteAllText($ini,$config)
    Set-Phase 1 startup
    $env:FC2VR_BODY_YAW_TEST_FILE="$out/pose.csv"
    & "$out/Launch-Test.ps1"
    $launch=Get-Content "$out/launch.json" -Raw|ConvertFrom-Json
    $gamePid=$launch.pid;$launchedAt=([DateTime]$launch.started).ToUniversalTime()
    $gameProcess=Get-Process -Id $gamePid -ErrorAction Stop
    [void]$gameProcess.Handle
    $writer=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+"$PSScriptRoot/body_yaw_writer.ps1"+'"'),'-GamePid',$gamePid,'-PoseFile',('"'+"$out/pose.csv"+'"'),'-ControlFile',('"'+"$out/control.json"+'"')) -RedirectStandardError "$out/writer-errors.log"
    Start-Sleep -Seconds 12
    & {
        # Bounded confirms select Story/Continue; desktop menus may be black.
        # Success still requires a real native player and both eye readbacks.
        $menuDeadline=[DateTime]::UtcNow.AddSeconds(180);$confirms=0
        do{
            if(!(Get-Process -Id $gamePid -ErrorAction SilentlyContinue)){throw 'Owned game exited during menu bootstrap'}
            if($writer.HasExited){throw 'Pose writer exited during menu bootstrap'}
            $fresh=(Get-Item -LiteralPath $log).LastWriteTimeUtc -ge $launchedAt
            $havePlayer=if($fresh){Get-Content -LiteralPath $log -Tail 60 | Select-String 'body-yaw-eye .*foot=1 .*have=1'}else{$null}
            if($havePlayer){break}
            $rendering=if($fresh){Get-Content -LiteralPath $log -Tail 60 | Select-String 'body-yaw-eye '}else{$null}
            if($rendering -and $confirms -lt 5){
                $focus=Start-Process $Probe -WindowStyle Hidden -Wait -PassThru -ArgumentList '--inspect-game-focus' -RedirectStandardOutput "$out/focus.json" -RedirectStandardError "$out/focus-errors.log"
                if(!$focus.ExitCode){
                    $identity=Get-Content "$out/focus.json" -Raw|ConvertFrom-Json
                    if($identity.pid -ne $gamePid){throw 'Probe selected a different game process'}
                    $key=Start-Process $Probe -WindowStyle Hidden -Wait -PassThru -ArgumentList '--menu-key','confirm' -RedirectStandardOutput "$out/menu-key.log" -RedirectStandardError "$out/menu-key-errors.log"
                    if(!$key.ExitCode){$confirms++}
                    "confirm=$confirms exit=$($key.ExitCode)"|Add-Content "$out/load-save.log"
                }
            }
            Start-Sleep -Seconds 10
        }while([DateTime]::UtcNow -lt $menuDeadline)
        if(!$havePlayer){throw 'Bounded menu bootstrap did not reach a native player'}
        # Player data precedes the actual camera placement during save loading.
        Start-Sleep -Seconds 30
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    do{
        if(!(Get-Process -Id $gamePid -ErrorAction SilentlyContinue)){throw 'Owned game exited'}
        $ready=Get-Content -LiteralPath $log -Tail 60 | Select-String 'body-yaw-eye .*foot=1 .*have=1'
        if(!$ready){Start-Sleep -Milliseconds 500}
    }while(!$ready -and [DateTime]::UtcNow -lt $deadline)
    if(!$ready){throw 'No on-foot camera readback; headset/session may be inactive'}
    Set-Phase 2 zero;Start-Sleep -Seconds 3
    Set-Phase 3 left90 90;Start-Sleep -Seconds 3
    Set-Phase 4 right90 -90;Start-Sleep -Seconds 3
    Set-Phase 5 back179 179;Start-Sleep -Seconds 3
    Set-Phase 6 wrapMinus179 -179;Start-Sleep -Seconds 3
    Set-Phase 7 pitchRoll -179 40 30;Start-Sleep -Seconds 3
    Set-Phase 8 trackingLost 20 0 0 0;Start-Sleep -Seconds 3
    Set-Phase 9 ineligible -20 0 0 1 0;Start-Sleep -Seconds 3
    Set-Phase 10 recovered 0;Start-Sleep -Seconds 3
    Set-Phase 11 stale 0 0 0 1 1 0 $true;Start-Sleep -Seconds 3
    Set-Phase 12 recentered 0 0 0 1 1 1;Start-Sleep -Seconds 3
    Set-Phase 13 afterRecenter 45 0 0 1 1 1;Start-Sleep -Seconds 3
    if($GrassFix){
        Set-Phase 14 nonzeroRecenter 45 0 0 1 1 2;Start-Sleep -Seconds 3
        Set-Phase 15 afterNonzeroRecenter 90 0 0 1 1 2;Start-Sleep -Seconds 3
    }
    if($GrassMotion){
        Set-Phase 16 continuousHeadTurn 0 0 0 1 1 2;Start-Sleep -Seconds 8
    }
    if($writer.HasExited){throw 'Pose writer exited during test'}
    if($LeftWeapon){Set-Phase 17 palmInspection 0 0 0 1 1 2;Start-Sleep -Seconds 3}
    if($WeaponHandToggle){
        if(!$LeftWeapon){throw 'WeaponHandToggle run requires LeftWeapon as its initial checkpoint'}
        $expectedLeft=1
        foreach($phase in @(18,19,20,21)){
            $expectedLeft=1-$expectedLeft
            Set-Phase $phase ('handToggle'+$expectedLeft) 0 0 0 1 1 2
            $before=@(Get-Content -LiteralPath $log|Select-String 'weapon-hand changed left=').Count
            $identity=& $Probe --inspect-game-focus|ConvertFrom-Json
            if($identity.pid -ne $gamePid){throw 'Toggle probe selected another process'}
            & $Probe --weapon-hand-toggle
            if($LASTEXITCODE -ne 0){throw 'F1 probe failed'}
            $until=[DateTime]::UtcNow.AddSeconds(15)
            do{
                $events=@(Get-Content -LiteralPath $log|Select-String 'weapon-hand changed left=')
                if($events.Count -gt $before){break}
                Start-Sleep -Milliseconds 100
            }while([DateTime]::UtcNow -lt $until)
            if($events.Count -ne $before+1 -or $events[-1] -notmatch "weapon-hand changed left=$expectedLeft "){throw 'F1 did not produce exactly one expected hand transition'}
            Start-Sleep -Seconds 3
            & $Probe --capture-window "$out/hand-$phase.png" *> "$out/capture-$phase.log"
        }
    }
    & $Probe --capture-window "$out/final.png" *> "$out/capture.log"
    'Native run completed; trace assertions still required.' | Set-Content "$out/run-completed.txt"
}finally{
    $env:FC2VR_BODY_YAW_TEST_FILE=$oldFixture
    if($gameProcess -and !$gameProcess.HasExited){$gameProcess.Kill();[void]$gameProcess.WaitForExit(5000)}
    if($writer -and !$writer.HasExited){$writer.Kill()}
    Copy-Item -LiteralPath $log -Destination "$out/native.log" -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $dll -Destination "$out/test-dinput8.dll"
    Copy-Item -LiteralPath $ini -Destination "$out/test-fc2vr.ini"
    Copy-Item -LiteralPath "$out/before-dinput8.dll" -Destination $dll
    Copy-Item -LiteralPath "$out/before-fc2vr.ini" -Destination $ini
    Copy-Item -LiteralPath "$out/before-profile.xml" -Destination $trial.profile
    foreach($binary in $trial.original_binaries){if((Get-FileHash -LiteralPath $binary.path).Hash -ne $binary.sha256){throw "Original binary changed: $($binary.path)"}}
    if((Get-FileHash $dll).Hash -ne (Get-FileHash "$out/before-dinput8.dll").Hash){throw 'DLL restoration failed'}
}
