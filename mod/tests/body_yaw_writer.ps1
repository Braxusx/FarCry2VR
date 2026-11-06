param([int]$GamePid,[string]$PoseFile,[string]$ControlFile)
$ErrorActionPreference='Stop'
$counter=0
$lastPhase=0;$phaseStart=[Diagnostics.Stopwatch]::StartNew()
while(Get-Process -Id $GamePid -ErrorAction SilentlyContinue){
  try{
    $stream=[IO.File]::Open($ControlFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=New-Object IO.StreamReader($stream)
    try{$phase=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
    if($phase.id -ne $lastPhase){$lastPhase=$phase.id;$phaseStart.Restart()}
    if(!$phase.freeze){
        $counter++
        $sequence=[long]$phase.id*100000+$counter
        $yaw=$phase.yaw
        if($phase.id -eq 16){$yaw=90*[Math]::Sin($phaseStart.Elapsed.TotalSeconds*1.5)}
        $line='{0},{1},{2},{3},{4},{5},{6},{7}' -f $GamePid,$sequence,$yaw,$phase.pitch,$phase.roll,$phase.tracked,$phase.allowed,$phase.reset
        [IO.File]::WriteAllText(($PoseFile+'.new'),$line)
        if(Test-Path -LiteralPath $PoseFile){[IO.File]::Replace(($PoseFile+'.new'),$PoseFile,($PoseFile+'.previous'))}else{[IO.File]::Move(($PoseFile+'.new'),$PoseFile)}
    }
  }catch [IO.IOException]{
    # Windows file replacement / scanners can briefly hold an exclusive handle.
    # Leave the old sequence untouched: native authority expires after 100 ms.
  }
    Start-Sleep -Milliseconds 25
}
