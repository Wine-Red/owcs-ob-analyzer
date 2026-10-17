param(
  [Parameter(Mandatory)][string]$ObsPath,
  [string]$PluginPath=(Join-Path (Split-Path $PSScriptRoot -Parent) '.build/Release/owcs-ob-analyzer.dll')
)
$ErrorActionPreference='Stop'
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Install Visual Studio C++ build tools'}
$toolset=(Get-Content (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt')).Trim()
$dump=Join-Path $vs "VC/Tools/MSVC/$toolset/bin/Hostx64/x64/dumpbin.exe"
$imports=& $dump /nologo /imports $PluginPath
if($LASTEXITCODE){throw 'Cannot inspect plugin imports'}
$module=''
$checked=0
$missing=@()
$exports=@{}
foreach($line in $imports){
  if($line -match '^\s+(\S+\.dll)\s*$'){
    $module=$Matches[1]
    if($module -notmatch '^(Qt6.*|obs|obs-frontend-api)\.dll$'){$module='';continue}
    $dll=Join-Path $ObsPath "bin/64bit/$module"
    if(!(Test-Path -LiteralPath $dll)){throw "Missing host dependency: $dll"}
    $symbols=& $dump /nologo /exports $dll
    if($LASTEXITCODE){throw "Cannot inspect $dll"}
    $exports[$module]=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($symbol in $symbols){
      if($symbol -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)'){
        [void]$exports[$module].Add($Matches[1])
      }
    }
  }elseif($module -and $line -match '^\s+[0-9A-F]+\s+(\S+)\s*$'){
    $symbol=$Matches[1]
    $checked++
    if(!$exports[$module].Contains($symbol)){$missing+="$module : $symbol"}
  }
}
if($checked -lt 50){throw "Incomplete import scan: only $checked symbols"}
if($missing.Count){throw "Missing host exports ($($missing.Count)):`n$($missing -join "`n")"}
Write-Host "PASS: $checked OBS/Qt imports resolved against $ObsPath"
