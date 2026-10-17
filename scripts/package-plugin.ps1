param([string]$ObsPath='', [switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$SkipBuild){& (Join-Path $PSScriptRoot 'build-plugin.ps1') -ObsPath $ObsPath}
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$projectText=Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if($projectText -notmatch 'project\(owcs-ob-analyzer VERSION ([0-9.]+)'){throw 'Project version not found'}
$version=$Matches[1]
$stage=Join-Path $root ('.build/package-'+[guid]::NewGuid().ToString('N'))
& $cmake --install (Join-Path $root '.build') --config Release --prefix $stage
if($LASTEXITCODE){throw 'Staging failed'}
Copy-Item -LiteralPath (Join-Path $root 'README_USER.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'packaging/Install.ps1'),(Join-Path $root 'packaging/Uninstall.ps1') -Destination $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'docs'),(Join-Path $stage 'schema') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs/STATE_API.md'),(Join-Path $root 'docs/RESOURCE_PACKS.md'),(Join-Path $root 'docs/CLIENT_PERKS.md') -Destination (Join-Path $stage 'docs')
Copy-Item -Path (Join-Path $root 'schema/*.json') -Destination (Join-Path $stage 'schema')
$files=@(Get-ChildItem -LiteralPath (Join-Path $stage 'obs-plugins'),(Join-Path $stage 'data') -Recurse -File | Sort-Object FullName | ForEach-Object {@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
$baseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'obs-baseline.json') -Raw | ConvertFrom-Json
@{version=$version;platform='windows-x64';obs_minimum=$baseline.obs_version;qt_build=$baseline.qt_version;files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8
$release=Join-Path $root 'release'
New-Item -ItemType Directory -Force $release | Out-Null
$zip=Join-Path $release "OWCS-OB-Analyzer-OBS-$version-windows-x64.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$source=Join-Path $release "OWCS-OB-Analyzer-OBS-$version-source.zip"
$sourcePaths=@('CMakeLists.txt','native','data','assets','tests','scripts','packaging','docs','schema','.github','.gitignore','.gitattributes','.clang-format','README.md','README_USER.md','LICENSE') | ForEach-Object {Join-Path $root $_}
$sourceStage=$stage+'-source'
New-Item -ItemType Directory -Force $sourceStage | Out-Null
foreach($path in $sourcePaths){
  $entries=if(Test-Path -LiteralPath $path -PathType Container){Get-ChildItem -LiteralPath $path -Recurse -File}else{Get-Item -LiteralPath $path}
  foreach($entry in $entries){
    if($entry.FullName -match '(__pycache__|\.egg-info|\.pytest_cache)' -or $entry.Extension -eq '.pyc'){continue}
    $relative=$entry.FullName.Substring($root.Length+1)
    $copy=Join-Path $sourceStage $relative
    New-Item -ItemType Directory -Force (Split-Path $copy -Parent) | Out-Null
    Copy-Item -LiteralPath $entry.FullName -Destination $copy
  }
}
Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $source -Force
Get-FileHash -LiteralPath $zip,$source -Algorithm SHA256 | ForEach-Object {"$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_.Path))"} | Set-Content -LiteralPath (Join-Path $release "OWCS-OB-Analyzer-OBS-$version-SHA256SUMS.txt") -Encoding ascii
Write-Host "Package: $zip"
Write-Host "Source:  $source"
