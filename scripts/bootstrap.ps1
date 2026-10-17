param([string]$ObsPath='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$deps=Join-Path $root '.deps'
New-Item -ItemType Directory -Force $deps | Out-Null
$baseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'obs-baseline.json') -Raw | ConvertFrom-Json
$items=@($baseline.downloads)+@(
  @{name='ort';file='ort.zip';url='https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip';sha='78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581';ready='ort/onnxruntime-win-x64-1.20.1/include/onnxruntime_cxx_api.h'},
  @{name='opencv';file='opencv.exe';url='https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe';sha='bff38466091c313dac21a0b73eea8278316a89c1d434c6f0b10697e087670168';ready='opencv/opencv/build/x64/vc16/lib/OpenCVConfig.cmake'}
)
# Always link against the baseline runtime. ObsPath selects only the test host.
foreach($item in $items){
  $file=Join-Path $deps $item.file
  if(!(Test-Path -LiteralPath $file)){New-Item -ItemType Directory -Force (Split-Path $file -Parent) | Out-Null;Write-Host "Downloading $($item.name)";Invoke-WebRequest -Uri $item.url -OutFile $file}
  if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha){throw "SHA256 mismatch: $file"}
  if(!(Test-Path -LiteralPath (Join-Path $deps $item.ready))){
    $dest=Join-Path $deps $item.name
    if($item.name -eq 'opencv'){
      $process=Start-Process -FilePath $file -ArgumentList '-y',('-o"'+$dest+'"') -WindowStyle Hidden -Wait -PassThru
      if($process.ExitCode){throw 'OpenCV extraction failed'}
    }else{Expand-Archive -LiteralPath $file -DestinationPath $dest -Force}
  }
}
& (Join-Path $PSScriptRoot 'prepare-obs-sdk.ps1')
Write-Host "Dependencies ready. Build baseline: OBS $($baseline.obs_version), Qt $($baseline.qt_version)"
