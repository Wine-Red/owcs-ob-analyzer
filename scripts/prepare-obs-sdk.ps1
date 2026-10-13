param([Parameter(Mandatory)][string]$ObsPath)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio 2022 C++ build tools' }
$toolset=(Get-Content (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt')).Trim()
$tools=Join-Path $vs "VC/Tools/MSVC/$toolset/bin/Hostx64/x64"
$sdk=Join-Path $root '.deps/obs-sdk'
New-Item -ItemType Directory -Force $sdk | Out-Null
foreach($name in @('obs','obs-frontend-api')) {
  $dll=Join-Path $ObsPath "bin/64bit/$name.dll"
  if (!(Test-Path -LiteralPath $dll)) { throw "Missing $dll" }
  $exports=& (Join-Path $tools 'dumpbin.exe') /nologo /exports $dll
  if ($LASTEXITCODE) { throw 'dumpbin failed' }
  $symbols=@($exports | ForEach-Object { if($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] } })
  if($symbols.Count -lt 10) { throw "Invalid exports: $dll" }
  $def=Join-Path $sdk "$name.def"
  @("LIBRARY $name.dll",'EXPORTS')+$symbols | Set-Content -LiteralPath $def -Encoding ascii
  & (Join-Path $tools 'lib.exe') /nologo "/def:$def" /machine:x64 "/out:$sdk/$name.lib"
  if ($LASTEXITCODE) { throw 'Import library generation failed' }
}
$ort=Join-Path $root '.deps/ort/onnxruntime-win-x64-1.20.1/lib/onnxruntime.dll'
if (!(Test-Path -LiteralPath $ort)) { throw 'Download dependencies first' }
@('LIBRARY owcs-onnxruntime.dll','EXPORTS','OrtGetApiBase') | Set-Content -LiteralPath (Join-Path $sdk 'owcs-onnxruntime.def') -Encoding ascii
& (Join-Path $tools 'lib.exe') /nologo "/def:$sdk/owcs-onnxruntime.def" /machine:x64 "/out:$sdk/owcs-onnxruntime.lib"
if ($LASTEXITCODE) { throw 'ONNX import library generation failed' }
$cv=Join-Path $root '.deps/opencv/opencv/build/x64/vc16/bin/opencv_world4100.dll'
$exports=& (Join-Path $tools 'dumpbin.exe') /nologo /exports $cv
if($LASTEXITCODE){throw 'OpenCV exports failed'}
$symbols=@($exports | ForEach-Object {if($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)'){$Matches[1]}})
@('LIBRARY owcs-opencv4100.dll','EXPORTS')+$symbols | Set-Content -LiteralPath (Join-Path $sdk 'owcs-opencv4100.def') -Encoding ascii
& (Join-Path $tools 'lib.exe') /nologo "/def:$sdk/owcs-opencv4100.def" /machine:x64 "/out:$sdk/owcs-opencv4100.lib"
if($LASTEXITCODE){throw 'OpenCV import library generation failed'}
