# Builds the port: extract the DOL from the disc, analyze, recompile, compile.
#   powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-Disc <iso>] [-Configure] [-Target blitz_headless] [-SkipRecomp]
param(
  [string]$Disc = "NFL Blitz 2003 (USA).iso",
  [switch]$Configure,
  [switch]$SkipRecomp,
  [string]$Target = "blitz_headless"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. "$PSScriptRoot\vsenv.ps1"

if (-not $SkipRecomp) {
  if (-not (Test-Path "orig/main.dol")) { python -I tools/gcdisc.py $Disc orig; if ($LASTEXITCODE) { exit 1 } }
  New-Item -ItemType Directory -Force build/analysis | Out-Null
  python -I tools/recomp/analyze.py orig/main.dol build/analysis/functions.json; if ($LASTEXITCODE) { exit 1 }
  python -I tools/recomp/recomp.py orig/main.dol build/analysis/functions.json config/sdk_symbols.txt,config/symbols.txt config/hooks.txt build/recomp_gen; if ($LASTEXITCODE) { exit 1 }
}

$bdir = "build/windows-clang"
if ($Configure -or -not (Test-Path "$bdir/build.ninja")) {
  cmake -S . -B $bdir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
  if ($LASTEXITCODE) { exit 1 }
}
cmake --build $bdir --target $Target
exit $LASTEXITCODE
