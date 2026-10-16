param([string]$Archive='')
$ErrorActionPreference='Stop'
if(!$Archive){
  $resourceVersion=(Get-Content -LiteralPath (Join-Path $PSScriptRoot '../assets/recognition/manifest.json') -Raw | ConvertFrom-Json).resource_version
  $Archive=Join-Path $PSScriptRoot "../release/OWCS-Recognition-$resourceVersion.zip"
}
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('owcs-resource-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$package=Join-Path $testRoot 'pack'
$obs=Join-Path $testRoot 'obs'
$models=Join-Path $obs 'data/obs-plugins/owcs-ob-analyzer/models'
Expand-Archive -LiteralPath $Archive -DestinationPath $package
New-Item -ItemType Directory -Force (Join-Path $obs 'bin/64bit'),$models | Out-Null
Set-Content -LiteralPath (Join-Path $obs 'bin/64bit/obs64.exe') -Value 'isolated fixture only'
Set-Content -LiteralPath (Join-Path $models 'manifest.json') -Value '{"asset_format":3,"resource_version":"before"}'
Set-Content -LiteralPath (Join-Path $models 'prior.marker') -Value 'old model directory'
$before=(Get-FileHash -LiteralPath (Join-Path $models 'manifest.json')).Hash
# Only this fixture test substitutes the process inventory. The installer has no bypass switch.
function Get-Process { param($Name) @() }
$installer=Join-Path $package 'Install-Resources.ps1'
$manifestFile=Join-Path $package 'manifest.json'
$original=Get-Content -LiteralPath $manifestFile -Raw
function Expect-Rejection([string]$label) {
  $rejected=$false
  try {& $installer -ObsPath $obs} catch {$rejected=$true}
  if(!$rejected){throw "Expected rejection: $label"}
  if((Get-FileHash -LiteralPath (Join-Path $models 'manifest.json')).Hash -ne $before){throw "Changed live models on rejection: $label"}
}
$m=$original | ConvertFrom-Json
$m.asset_format=99
$m | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestFile
Expect-Rejection 'format mismatch'
$m=$original | ConvertFrom-Json
$m.files[0].sha256='0000000000000000000000000000000000000000000000000000000000000000'
$m | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestFile
Expect-Rejection 'checksum mismatch'
$m=$original | ConvertFrom-Json
$m.files[0].path='models/../../outside'
$m | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestFile
Expect-Rejection 'path escape'
Set-Content -LiteralPath $manifestFile -Value $original
& $installer -ObsPath $obs
if((Get-Content -LiteralPath (Join-Path $models 'manifest.json') -Raw | ConvertFrom-Json).resource_version -ne ($original | ConvertFrom-Json).resource_version){throw 'Wrong installed resource version'}
$backup=Get-ChildItem -LiteralPath (Join-Path $obs 'owcs-plugin-backups') -Directory | Select-Object -First 1
if((Get-FileHash -LiteralPath (Join-Path $backup.FullName 'models/manifest.json')).Hash -ne $before){throw 'Backup mismatch'}
if(!(Test-Path -LiteralPath (Join-Path $backup.FullName 'models/prior.marker'))){throw 'Incomplete backup'}
Write-Host "5 resource installer checks passed. Isolated evidence: $testRoot"
