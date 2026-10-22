# Builds only the game module (blitz_game.dll) from a disc image or main.dol: extract the
# executable, analyze, recompile, compile. Needs Python 3 and clang-cl (Visual Studio's or LLVM's);
# the runtime (NFLBlitz2003.exe) is not rebuilt. This is the step a first-run setup performs on
# the player's machine.
#   powershell -ExecutionPolicy Bypass -File tools/make_module.ps1 [-Disc <iso>] [-Dol <main.dol>] [-Out <dll>]
param(
  [string]$Disc = "NFL Blitz 2003 (USA).iso",
  [string]$Dol = "",
  [string]$Out = "build/windows-clang/blitz_game.dll",
  [string]$Work = "build/module"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue) -or -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
  . "$PSScriptRoot\vsenv.ps1"
}

# Python 3: the py launcher or python on PATH (not the Microsoft Store stub).
$py = @()
$cmd = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmd) { $py = @($cmd.Source, "-3") }
else {
  $cmd = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cmd) { $py = @($cmd.Source) }
}
if ($py.Count) { & $py[0] $py[1..9] -c "import sys; sys.exit(sys.version_info < (3, 8))" 2>$null; if ($LASTEXITCODE) { $py = @() } }
if (-not $py.Count) { Write-Error "Python 3.8 or newer is required (python.org)."; exit 1 }
function Invoke-Python { & $py[0] $py[1..9] -I @args }

# The recompiler rewrites every chunk; chunks left by another version must not be compiled in.
if (Test-Path "$Work/gen") { Remove-Item -Recurse -Force "$Work/gen" }
New-Item -ItemType Directory -Force "$Work/gen" | Out-Null
if (-not $Dol) {
  Invoke-Python tools/gcdisc.py $Disc $Work --dol-only; if ($LASTEXITCODE) { exit 1 }
  $Dol = "$Work/main.dol"
}
Invoke-Python tools/recomp/analyze.py $Dol "$Work/functions.json"; if ($LASTEXITCODE) { exit 1 }
Invoke-Python tools/recomp/recomp.py $Dol "$Work/functions.json" "config/sdk_symbols.txt,config/symbols.txt" config/hooks.txt "$Work/gen"
if ($LASTEXITCODE) { exit 1 }

# A small ninja file: parallel and incremental (unchanged chunks are not recompiled).
$rt = (Resolve-Path src/runtime).Path -replace '\\', '/'
$gen = (Resolve-Path "$Work/gen").Path -replace '\\', '/'
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
$outAbs = [IO.Path]::GetFullPath($Out) -replace '\\', '/'
$esc = { param($p) $p -replace ':', '$:' -replace ' ', '$ ' }
$lines = @(
  "cxxflags = /nologo /DWIN32 /D_WINDOWS /EHsc /O2 /Ob1 /DNDEBUG -std:c++20 -MD -I`"$rt`" -I`"$gen`" -Wno-unused-label -Wno-unused-variable -Wno-unreachable-code -Wno-parentheses-equality -Wno-tautological-compare",
  "rule cxx",
  "  command = clang-cl `$cxxflags /showIncludes -c `$in /Fo`$out",
  "  deps = msvc",
  "  description = CXX `$out",
  "rule dll",
  "  command = lld-link /nologo /DLL /OUT:`$out `$in",
  "  description = LINK `$out"
)
$objs = @()
$sources = @(Get-ChildItem "$Work/gen/*.cpp" | ForEach-Object { $_.FullName -replace '\\', '/' }) + "$rt/module_rt.cpp"
foreach ($src in $sources) {
  $obj = "obj/" + [IO.Path]::GetFileNameWithoutExtension($src) + ".obj"
  $lines += "build ${obj}: cxx $(& $esc $src)"
  $objs += $obj
}
New-Item -ItemType Directory -Force (Split-Path -Parent $outAbs) | Out-Null
$lines += "build $(& $esc $outAbs): dll $($objs -join ' ')"
Set-Content -Path "$Work/build.ninja" -Value $lines -Encoding ascii
ninja -C $Work
exit $LASTEXITCODE
