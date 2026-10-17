param([string]$ObsPath='', [switch]$SkipBootstrap)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$SkipBootstrap){& (Join-Path $PSScriptRoot 'bootstrap.ps1') -ObsPath $ObsPath}
$baseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'obs-baseline.json') -Raw | ConvertFrom-Json
if(!$ObsPath){$ObsPath=Join-Path $root ('.deps/'+$baseline.runtime_directory)}
$ObsPath=[IO.Path]::GetFullPath($ObsPath)
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Install Visual Studio 2022 with Desktop development with C++'}
$cmakeDir=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$cmake=Join-Path $cmakeDir 'cmake.exe'
$build=Join-Path $root '.build'
# Discard cached Qt package paths when switching from the former Qt 6.8 build.
& $cmake --fresh -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DOBS_INSTALL=$ObsPath"
if($LASTEXITCODE){throw 'CMake configure failed'}
& $cmake --build $build --config Release --parallel
if($LASTEXITCODE){throw 'Build failed'}
& (Join-Path $PSScriptRoot 'test-obs-imports.ps1') -ObsPath $ObsPath
& (Join-Path $cmakeDir 'ctest.exe') --test-dir $build -C Release --output-on-failure
if($LASTEXITCODE){throw 'Native tests failed'}
Write-Host "Built and tested: $build/Release/owcs-ob-analyzer.dll"
