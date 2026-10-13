param([Parameter(Mandatory)][string]$ObsPath)
$ErrorActionPreference='Stop'
$target=[IO.Path]::GetFullPath($ObsPath)
if(!(Test-Path -LiteralPath (Join-Path $target 'bin/64bit/obs64.exe'))){throw 'Select the OBS installation root'}
$running=Get-Process obs64 -ErrorAction SilentlyContinue
if($running | Where-Object {!$_.Path -or $_.Path -eq (Join-Path $target 'bin/64bit/obs64.exe')}){throw 'Close this OBS installation before updating resources'}
$models=Join-Path $target 'data/obs-plugins/owcs-ob-analyzer/models'
$current=Get-Content -LiteralPath (Join-Path $models 'manifest.json') -Raw | ConvertFrom-Json
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
if($current.asset_format -ne 2 -or $manifest.asset_format -ne 2 -or $manifest.type -ne 'owcs-recognition-pack'){throw 'Resource format mismatch. Install plugin 0.3.0 or later first'}
$expected=@('manifest.json','names.onnx','portraits-full.qz','ready.png','templates.qz','unselected-hud.png','unselected-panel.png')
$found=@()
foreach($entry in $manifest.files){
  $name=([string]$entry.path) -replace '^models/',''
  if($entry.path -ne "models/$name" -or $name -notin $expected -or $name -in $found){throw 'Unexpected or duplicate resource path'}
  $found+=$name
  $file=Join-Path $PSScriptRoot $entry.path
  if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256){throw "Checksum mismatch: $name"}
}
if($found.Count -ne $expected.Count){throw 'Incomplete resource pack'}
$incoming=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'models/manifest.json') -Raw | ConvertFrom-Json
if($incoming.asset_format -ne 2 -or $incoming.resource_version -ne $manifest.resource_version){throw 'Resource manifest mismatch'}
$stamp=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N')
$stage=Join-Path $target "data/obs-plugins/owcs-ob-analyzer/models-stage-$stamp"
$backup=Join-Path $target "owcs-plugin-backups/resources-$stamp/models"
# Verify directory move destinations stay inside the selected OBS installation.
foreach($path in @($models,$stage,$backup)){
  if(![IO.Path]::GetFullPath($path).StartsWith($target.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Path escapes OBS installation'}
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach($name in $expected){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "models/$name") -Destination (Join-Path $stage $name)}
New-Item -ItemType Directory -Path (Split-Path $backup -Parent) -Force | Out-Null
Move-Item -LiteralPath $models -Destination $backup
try {Move-Item -LiteralPath $stage -Destination $models}
catch {Move-Item -LiteralPath $backup -Destination $models;throw}
Write-Host "Installed resource pack $($manifest.resource_version). Restart OBS. Backup: $backup"
