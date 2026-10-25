param(
    [Parameter(Mandatory=$true)][string]$Audit,
    [Parameter(Mandatory=$true)][string]$TestExe
)
$ErrorActionPreference='Stop'
$auditPath=(Resolve-Path -LiteralPath $Audit).Path
$testPath=(Resolve-Path -LiteralPath $TestExe).Path
$assets=Get-Content -LiteralPath $auditPath -Raw | ConvertFrom-Json
$assetCount=0
foreach($asset in $assets) {
    if($asset.bones.Count -lt 1 -or $asset.bones.Count -gt 512) { throw 'Unsupported audit count.' }
    $rows=[System.Collections.Generic.List[string]]::new()
    $rows.Add([string]$asset.bones.Count)
    $expected=0
    foreach($bone in $asset.bones) {
        if($bone.index -ne $expected -or $bone.links.Count -ne 3 -or !$bone.name) { throw 'Incomplete or unordered audit.' }
        ++$expected
        $rows.Add(([string]$bone.links[2])+' '+$bone.name)
        $m=$bone.importer_world
        if($m.Count -ne 4 -or @($m | Where-Object { $_.Count -ne 4 }).Count) { throw 'Missing importer transform.' }
        # Importer matrix is column-vector: transpose basis columns into row axes.
        # This is an offline importer-pose fixture, not proof of runtime pose convention.
        $values=@($m[0][0],$m[1][0],$m[2][0],$m[0][1],$m[1][1],$m[2][1],$m[0][2],$m[1][2],$m[2][2],$m[0][3],$m[1][3],$m[2][3])
        $rows.Add((($values | ForEach-Object { ([double]$_).ToString('R',[System.Globalization.CultureInfo]::InvariantCulture) }) -join ' '))
    }
    Write-Output ('Asset: '+[System.IO.Path]::GetFileName($asset.path))
    $rows | & $testPath replay_pose
    if($LASTEXITCODE -ne 0) { throw "Asset replay failed with exit $LASTEXITCODE." }
    ++$assetCount
}
if(!$assetCount) { throw 'Empty audit is not coverage.' }
Write-Output "Replayed $assetCount assets; 2 arms x 2 pose controls per asset. No native execution."
