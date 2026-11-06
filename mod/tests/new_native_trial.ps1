param(
 [Parameter(Mandatory)][string]$GameBin,
 [Parameter(Mandatory)][string]$Profile,
 [Parameter(Mandatory)][string]$RuntimeJson,
 [Parameter(Mandatory)][string]$Output,
 [string]$Executable
)
# Read-only inventory of an existing installation. No install or LAA patching.
$ErrorActionPreference='Stop'
$bin=(Resolve-Path -LiteralPath $GameBin).Path
$profilePath=(Resolve-Path -LiteralPath $Profile).Path
$runtime=(Resolve-Path -LiteralPath $RuntimeJson).Path
if(!$Executable){$Executable=Join-Path $bin 'FarCry2VR.exe';if(!(Test-Path -LiteralPath $Executable)){$Executable=Join-Path $bin 'FarCry2.exe'}}
$exe=(Resolve-Path -LiteralPath $Executable).Path
if([IO.Path]::GetFileName($exe) -notin @('FarCry2.exe','FarCry2VR.exe')){throw 'Probe supports FarCry2.exe or FarCry2VR.exe'}
if(Test-Path -LiteralPath $Output){throw 'Choose a fresh trial directory'}
$originals=@(foreach($name in @('FarCry2.exe','FC2.dll','Dunia.dll')){$path=Join-Path $bin $name;@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}})
$files=@(foreach($name in @('dinput8.dll','openxr_loader.dll','fc2vr.ini')){$path=Join-Path $bin $name;@{name=$name;sha256=(Get-FileHash -LiteralPath $path).Hash}})
if([IO.Path]::GetFileName($exe) -eq 'FarCry2VR.exe'){$files+=@{name='FarCry2VR.exe';sha256=(Get-FileHash -LiteralPath $exe).Hash}}
$out=(New-Item -ItemType Directory -Path $Output).FullName
@{bin=$bin;profile=$profilePath;xr_runtime=$runtime;executable=$exe;original_binaries=$originals;files=$files}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $out 'test-manifest.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'launch_native_game.ps1') -Destination (Join-Path $out 'Launch-Test.ps1')
"Created read-only native trial inventory: $out"
