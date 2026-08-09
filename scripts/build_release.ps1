param(
    [string]$Python,
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$VenvPython = Join-Path $RepoRoot ".venv\Scripts\python.exe"
$PyProject = Join-Path $RepoRoot "pyproject.toml"
$Spec = Join-Path $RepoRoot "packaging\pyinstaller\owcs_ob_analyzer.spec"
$WorkDir = Join-Path $RepoRoot "build\pyinstaller"
$DistRoot = Join-Path $RepoRoot "dist"
$ArtifactDir = Join-Path $DistRoot "OWCSOBAnalyzer"
$ReleaseRoot = Join-Path $RepoRoot "release"

function Assert-ChildPath([string]$Candidate, [string]$Parent) {
    $CandidateFull = [System.IO.Path]::GetFullPath($Candidate)
    $ParentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $CandidateFull.StartsWith($ParentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $CandidateFull"
    }
}

if (-not $Python) {
    if (-not (Test-Path -LiteralPath $VenvPython)) {
        & py -3.10 -m venv (Join-Path $RepoRoot ".venv")
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to create the Python 3.10 build environment"
        }
    }
    $Python = $VenvPython
}

if (-not (Test-Path -LiteralPath $Python)) {
    throw "Python was not found: $Python"
}

$VersionMatch = Select-String -Path $PyProject -Pattern '^version\s*=\s*"([^"]+)"$'
if (-not $VersionMatch) {
    throw "Could not read the version from pyproject.toml"
}
$Version = $VersionMatch.Matches[0].Groups[1].Value
$ZipPath = Join-Path $ReleaseRoot "OWCS-OB-Analyzer-$Version-win-x64-portable.zip"

if (-not $SkipInstall) {
    & $Python -m pip install --upgrade pip
    if ($LASTEXITCODE -ne 0) { throw "Failed to update pip" }
    & $Python -m pip install -e "$RepoRoot[release]"
    if ($LASTEXITCODE -ne 0) { throw "Failed to install build dependencies" }
}

foreach ($Target in @($WorkDir, $ArtifactDir, $ZipPath)) {
    Assert-ChildPath $Target $RepoRoot
    if (Test-Path -LiteralPath $Target) {
        Remove-Item -LiteralPath $Target -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $WorkDir, $DistRoot, $ReleaseRoot | Out-Null

& $Python -m PyInstaller `
    --noconfirm `
    --clean `
    --workpath $WorkDir `
    --distpath $DistRoot `
    $Spec
if ($LASTEXITCODE -ne 0) {
    throw "PyInstaller build failed"
}

Copy-Item -LiteralPath (Join-Path $RepoRoot "README_USER.md") -Destination (Join-Path $ArtifactDir "README.md")
New-Item -ItemType Directory -Force -Path (Join-Path $ArtifactDir "docs") | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot "docs\STATE_API.md") -Destination (Join-Path $ArtifactDir "docs\STATE_API.md")
Copy-Item -LiteralPath (Join-Path $RepoRoot "schema\player-state-v1.schema.json") -Destination (Join-Path $ArtifactDir "docs\player-state-v1.schema.json")

Compress-Archive -Path (Join-Path $ArtifactDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal

$ExePath = Join-Path $ArtifactDir "OWCSOBAnalyzer.exe"
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Build completed without the expected executable: $ExePath"
}

$ZipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ZipPath).Hash
Write-Host "EXE: $ExePath"
Write-Host "ZIP: $ZipPath"
Write-Host "SHA256: $ZipHash"
