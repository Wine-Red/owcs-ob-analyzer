param([Parameter(Mandatory)][string]$ObsPath)
$ErrorActionPreference='Stop'
$target=[IO.Path]::GetFullPath($ObsPath)
$exe=Join-Path $target 'bin/64bit/obs64.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Select the OBS installation root containing bin/64bit/obs64.exe'}
$version=(Get-Item -LiteralPath $exe).VersionInfo.FileVersion
if([version]($version -replace '[^0-9.].*$','') -lt [version]'32.2.2'){throw 'This package requires OBS 32.2.2 or newer'}
if(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}){throw 'Close this OBS installation before installing. Your scenes and settings will be preserved.'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
$files=@()
foreach($entry in $manifest.files){
  $relative=[string]$entry.path
  if($relative -notmatch '^(obs-plugins/64bit/owcs-[^/]+\.dll|data/obs-plugins/owcs-ob-analyzer/.+)$'){throw "Unexpected package path: $relative"}
  $from=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $relative))
  $to=[IO.Path]::GetFullPath((Join-Path $target $relative))
  if(!$from.StartsWith($PSScriptRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or !$to.StartsWith($target.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Path escapes installation root'}
  if((Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash -ne $entry.sha256){throw "Checksum mismatch: $relative"}
  $files+=@{from=$from;to=$to;relative=$relative}
}
$backup=Join-Path $target ('owcs-plugin-backups/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
foreach($item in $files){
  if(Test-Path -LiteralPath $item.to){$old=Join-Path $backup $item.relative;New-Item -ItemType Directory -Force (Split-Path $old -Parent) | Out-Null;Copy-Item -LiteralPath $item.to -Destination $old}
  New-Item -ItemType Directory -Force (Split-Path $item.to -Parent) | Out-Null
  Copy-Item -LiteralPath $item.from -Destination $item.to -Force
}
Write-Host "Installed OWCS OB Analyzer $($manifest.version). Start OBS and add the OWCS video filter to a source."
