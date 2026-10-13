param([string]$ObsPath='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$deps=Join-Path $root '.deps'
New-Item -ItemType Directory -Force $deps | Out-Null
$items=@(
  @{name='obs';file='obs.zip';url='https://codeload.github.com/obsproject/obs-studio/zip/refs/tags/32.2.2';sha='f15f001f1fa526405318835f44f9910046502f496ebc3a30d5296a5018b831aa';ready='obs/obs-studio-32.2.2/libobs/obs.h'},
  @{name='qt6';file='qt6.zip';url='https://github.com/obsproject/obs-deps/releases/download/2025-07-11/windows-deps-qt6-2025-07-11-x64.zip';sha='0e76bf0555dd5382838850b748d3dcfab44a1e1058441309ab54e1a65b156d0a';ready='qt6/lib/cmake/Qt6/Qt6Config.cmake'},
  @{name='ort';file='ort.zip';url='https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip';sha='78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581';ready='ort/onnxruntime-win-x64-1.20.1/include/onnxruntime_cxx_api.h'},
  @{name='opencv';file='opencv.exe';url='https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe';sha='bff38466091c313dac21a0b73eea8278316a89c1d434c6f0b10697e087670168';ready='opencv/opencv/build/x64/vc16/lib/OpenCVConfig.cmake'}
)
if(!$ObsPath){
  $items+=@{name='obs-runtime';file='obs-runtime.zip';url='https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Windows-x64.zip';sha='4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1';ready='obs-runtime/bin/64bit/obs64.exe'}
  $ObsPath=Join-Path $deps 'obs-runtime'
}
foreach($item in $items){
  $file=Join-Path $deps $item.file
  if(!(Test-Path -LiteralPath $file)){Write-Host "Downloading $($item.name)";Invoke-WebRequest -Uri $item.url -OutFile $file}
  if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha){throw "SHA256 mismatch: $file"}
  if(!(Test-Path -LiteralPath (Join-Path $deps $item.ready))){
    $dest=Join-Path $deps $item.name
    if($item.name -eq 'opencv'){
      $process=Start-Process -FilePath $file -ArgumentList '-y',('-o"'+$dest+'"') -WindowStyle Hidden -Wait -PassThru
      if($process.ExitCode){throw 'OpenCV extraction failed'}
    }else{Expand-Archive -LiteralPath $file -DestinationPath $dest -Force}
  }
}
& (Join-Path $PSScriptRoot 'prepare-obs-sdk.ps1') -ObsPath $ObsPath
Write-Host "Dependencies ready. OBS path: $ObsPath"
