# Builds the port: extract the DOL from the disc, analyze, recompile, compile.
#   powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-Disc <iso>] [-Configure] [-Target blitz_headless] [-SkipRecomp] [-BuildDir dir]
param(
  [string]$Disc = "NFL Blitz 2003 (USA).iso",
  [switch]$Configure,
  [switch]$SkipRecomp,
  [string]$Target = "blitz_headless",
  [string]$BuildDir = "build/windows-clang"  # a second directory lets two builds/test runs proceed side by side
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. "$PSScriptRoot\vsenv.ps1"

# Local fixes to Aurora (patches/aurora/*.patch), applied once to the submodule checkout.
# (git's expected stderr must not stop the script, hence Continue around these calls.)
$ErrorActionPreference = "Continue"
foreach ($patch in Get-ChildItem patches/aurora/*.patch -ErrorAction SilentlyContinue) {
  git -C extern/aurora apply --ignore-whitespace --reverse --check $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }  # already applied
  git -C extern/aurora apply --ignore-whitespace $patch.FullName
  if ($LASTEXITCODE) { Write-Host "cannot apply $($patch.Name) to extern/aurora"; exit 1 }
  Write-Host "applied $($patch.Name)"
}
$ErrorActionPreference = "Stop"

if (-not $SkipRecomp) {
  if (-not (Test-Path "orig/main.dol")) { python -I tools/gcdisc.py $Disc orig; if ($LASTEXITCODE) { exit 1 } }
  New-Item -ItemType Directory -Force build/analysis | Out-Null
  python -I tools/recomp/analyze.py orig/main.dol build/analysis/functions.json; if ($LASTEXITCODE) { exit 1 }
  python -I tools/recomp/recomp.py orig/main.dol build/analysis/functions.json config/sdk_symbols.txt,config/symbols.txt config/hooks.txt build/recomp_gen; if ($LASTEXITCODE) { exit 1 }
}

$bdir = $BuildDir
if ($Configure -or -not (Test-Path "$bdir/build.ninja")) {
  cmake -S . -B $bdir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
  if ($LASTEXITCODE) { exit 1 }
}
cmake --build $bdir --target $Target
exit $LASTEXITCODE
