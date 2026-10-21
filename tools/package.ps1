# Assembles a distributable folder (dist/NFLBlitz2003) WITHOUT any game code: the runtime and its
# libraries plus the files the first start needs to compile the game module from the player's own
# disc (recompiler, configuration, module headers). Build the runtime first (tools/build.ps1).
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1 [-Out dist/NFLBlitz2003] [-Zip]
#   (-Zip also writes <Out>-windows-x64.zip, the release archive: ~17 MB)
param([string]$Out = "dist/NFLBlitz2003", [switch]$Zip)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bin = "build/windows-clang"
if (-not (Test-Path "$bin/NFLBlitz2003.exe")) { throw "build NFLBlitz2003.exe first (tools/build.ps1 -Target blitz)" }
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }

function Put($src, $dst) {
  $d = Join-Path $Out $dst
  New-Item -ItemType Directory -Force (Split-Path -Parent $d) | Out-Null
  Copy-Item $src $d
}

Put "$bin/NFLBlitz2003.exe" "NFLBlitz2003.exe"
Get-ChildItem "$bin/*.dll" | Where-Object { $_.Name -ne "blitz_game.dll" } | ForEach-Object { Put $_.FullName $_.Name }
foreach ($f in "tools/make_module.ps1", "tools/vsenv.ps1", "tools/gcdisc.py",
               "tools/recomp/analyze.py", "tools/recomp/dol.py", "tools/recomp/ppc.py", "tools/recomp/recomp.py",
               "config/hooks.txt", "config/sdk_symbols.txt", "config/symbols.txt",
               "src/runtime/ctx.h", "src/runtime/func_table.h", "src/runtime/module_abi.h", "src/runtime/module_rt.cpp",
               "README.md") {
  Put $f $f
}
# Third-party notices for the libraries in the runtime.
Put "extern/aurora/LICENSE" "licenses/aurora.txt"
$deps = "$bin/_deps"
foreach ($pair in @(("imgui-src/LICENSE.txt", "imgui.txt"), ("fmt-src/LICENSE", "fmt.txt"), ("xxhash-src/LICENSE", "xxhash.txt"),
                    ("zlib-src/LICENSE.md", "zlib.txt"), ("png-src/LICENSE", "libpng.txt"), ("zstd-src/LICENSE", "zstd.txt"),
                    ("abseil-cpp-src/LICENSE", "abseil.txt"), ("freetype-src/LICENSE.TXT", "freetype.txt"))) {
  if (Test-Path "$deps/$($pair[0])") { Put "$deps/$($pair[0])" "licenses/$($pair[1])" }
}
if (Test-Path "$deps/sdl3_prebuilt-src/licenses") {
  Get-ChildItem "$deps/sdl3_prebuilt-src/licenses" -File | ForEach-Object { Put $_.FullName "licenses/sdl3-$($_.Name)" }
}
# Nothing translated from the game may be in the package.
$bad = Get-ChildItem -Recurse $Out | Where-Object { $_.Name -match '^(blitz_game\.dll|recomp_\d+\.cpp|main\.dol)$' -or $_.Extension -in ".iso", ".rvz" }
if ($bad) { throw "game data in the package: $($bad.FullName -join ', ')" }
Write-Host "packaged $Out ($([math]::Round((Get-ChildItem -Recurse $Out | Measure-Object Length -Sum).Sum / 1MB)) MB)"
if ($Zip) {
  $zipPath = "$Out-windows-x64.zip"
  if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
  Compress-Archive -Path $Out -DestinationPath $zipPath
  Write-Host "zipped $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB, 1)) MB)"
}
