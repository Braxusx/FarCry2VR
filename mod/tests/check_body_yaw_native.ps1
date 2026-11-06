param([Parameter(Mandatory)][string]$Run,[ValidateSet(0,1)][int]$Enabled=1,[switch]$GrassRecenter)
$ErrorActionPreference='Stop'
if(!(Test-Path "$Run/run-completed.txt")){throw 'Native run did not complete'}
$lines=Get-Content "$Run/native.log"
if($Enabled -and !($lines -match 'body-yaw configured enabled=1 exactSteamImage=1')){throw 'Exact-build writer was not enabled'}
if($lines -match 'body-yaw apply .*accepted=0'){throw 'Native writer refused or readback failed'}
function Wrap([double]$a){[Math]::Atan2([Math]::Sin($a),[Math]::Cos($a))}
$rows=@(foreach($line in $lines){
    if($line -match 'body-yaw-eye frame=(\d+) sample=(\d+) yaw=([-\d.]+) pitch=([-\d.]+) roll=([-\d.]+) tracked=(\d) allowed=(\d) foot=(\d) eye=(\d) owner=(\S+) native=([-\d.]+) transferred=([-\d.]+) expected=([-\d.]+) observed=([-\d.]+) have=(\d) reference=(\d+)'){
        [pscustomobject]@{frame=[long]$Matches[1];sample=[long]$Matches[2];phase=[int][Math]::Floor([long]$Matches[2]/100000);yaw=[double]$Matches[3];tracked=[int]$Matches[6];allowed=[int]$Matches[7];foot=[int]$Matches[8];eye=[int]$Matches[9];native=[double]$Matches[11];transfer=[double]$Matches[12];expected=[double]$Matches[13];observed=[double]$Matches[14];have=[int]$Matches[15];reference=[long]$Matches[16]}
    }elseif($line -match 'body-yaw-eye'){throw 'Malformed camera record'}
})
$settled=@{}
$phases=@(2..13)
if($GrassRecenter){$phases+=14,15}
foreach($id in $phases){
    # Frozen-source phase 11 deliberately retains phase 10's sequence.
    if($id -eq 11){continue}
    $group=@($rows|Where-Object {$_.phase -eq $id -and $_.foot -and $_.have})
    if($group.Count -lt 8){throw "Missing native coverage for phase $id"}
    $cut=[int]($group.Count/2);$group=@($group|Select-Object -Skip $cut)
    if(@($group|Select-Object -ExpandProperty eye -Unique).Count -ne 2){throw "Missing stereo eye in phase $id"}
    foreach($r in $group){if([Math]::Abs((Wrap ($r.observed-$r.expected))) -gt 0.002){throw "Engine camera mismatch in phase $id"}}
    $settled[$id]=$group
}
$baseline=$settled[2][-1]
# The body is allowed to catch up over bounded steps. The view is not allowed
# an extra rotation during those steps, so inspect transitions as well.
foreach($r in @($rows|Where-Object {$_.phase -ge 2 -and $_.foot -and $_.have})){
    if([Math]::Abs((Wrap ($r.observed-$r.expected))) -gt 0.002){throw 'Engine camera mismatch during transition'}
    $target=$r.yaw*[Math]::PI/180
    if([Math]::Abs((Wrap ($r.observed-$baseline.observed-$target))) -gt 0.035){throw 'World view rotated incorrectly during transition'}
}
$characters=@(foreach($line in $lines){
    if($line -match 'body-yaw-native sample=(\d+) native=([-\d.]+) character=([-\d.]+) have=(\d) transferred=([-\d.]+)'){
        [pscustomobject]@{phase=[int][Math]::Floor([long]$Matches[1]/100000);native=[double]$Matches[2];character=[double]$Matches[3];have=[int]$Matches[4]}
    }elseif($line -match 'body-yaw-native'){throw 'Malformed character readback'}
})
if($Enabled){
    $characterBase=@($characters|Where-Object {$_.phase -eq 2 -and $_.have})
    if(!$characterBase.Count){throw 'No native character orientation readback'}
    $characterBase=$characterBase[-1].character
}
$results=@(foreach($id in @($phases|Where-Object {$_ -ne 11})){
    $group=$settled[$id];$last=$group[-1]
    $bodyTarget=0
    if($Enabled){
        if($id -in @(8,9)){$bodyTarget=-179*[Math]::PI/180}
        else{$bodyTarget=$last.yaw*[Math]::PI/180}
    }
    foreach($r in $group){
        if([Math]::Abs((Wrap ($r.native-$baseline.native-$bodyTarget))) -gt 0.035){throw "Body yaw mismatch in phase $id"}
        # Independent of the feature's internal expected angle: the native camera
        # must turn exactly once by the scripted physical head angle.
        $viewTarget=$r.yaw*[Math]::PI/180
        if([Math]::Abs((Wrap ($r.observed-$baseline.observed-$viewTarget))) -gt 0.035){throw "World view rotated incorrectly in phase $id"}
    }
    if($Enabled){
        $cg=@($characters|Where-Object {$_.phase -eq $id -and $_.have})
        if($cg.Count -lt 4){throw "Missing sustained character orientation in phase $id"}
        foreach($r in @($cg|Select-Object -Skip ([int]($cg.Count/2)))){
            if([Math]::Abs((Wrap ($r.character-$characterBase-$bodyTarget))) -gt 0.035){throw "Character rotation was not retained in phase $id"}
        }
    }
    [pscustomobject]@{phase=$id;samples=$group.Count;native=$last.native;view=$last.observed;transferred=$last.transfer;reference=$last.reference}
})
if($Enabled -and $settled[12][-1].reference -le $settled[10][-1].reference){throw 'Recenter did not advance reference'}
if($Enabled -and $GrassRecenter -and $settled[14][-1].reference -le $settled[13][-1].reference){throw 'Nonzero recenter did not advance reference'}
$stale=@($rows|Where-Object {$_.phase -eq 10 -and !$_.tracked})
if(!$stale.Count){throw 'Stale-source gate was not exercised'}
foreach($r in $stale){if([Math]::Abs((Wrap ($r.native-$baseline.native))) -gt 0.035){throw 'Body changed with stale source'}}
$results|ConvertTo-Json|Set-Content "$Run/assertions.json"
$results|Format-Table
Write-Output 'PASS: native body heading, single view rotation, both eye cameras, pitch/roll, tracking/mode/stale gates and recenter.'
