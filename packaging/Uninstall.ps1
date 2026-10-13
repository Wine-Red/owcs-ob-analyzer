param([Parameter(Mandatory)][string]$ObsPath)
$ErrorActionPreference='Stop'
$target=[IO.Path]::GetFullPath($ObsPath).TrimEnd('\')
$exe=Join-Path $target 'bin/64bit/obs64.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Invalid OBS installation'}
if(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}){throw 'Close OBS before uninstalling'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
foreach($entry in $manifest.files){
  $relative=[string]$entry.path
  if($relative -notmatch '^(obs-plugins/64bit/owcs-[^/]+\.dll|data/obs-plugins/owcs-ob-analyzer/.+)$'){throw 'Unexpected package path'}
  $path=[IO.Path]::GetFullPath((Join-Path $target $relative))
  if(!$path.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid uninstall path'}
  if(Test-Path -LiteralPath $path){
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $entry.sha256){Remove-Item -LiteralPath $path}
    else{Write-Warning "Preserved modified or newer file: $path"}
  }
}
Write-Host 'Plugin files removed. OBS scenes and plugin settings were preserved.'
