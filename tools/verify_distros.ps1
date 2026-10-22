# Runs tools/verify_linux.ps1 -Image for each distribution (Podman containers in WSL; see there): the
# Linux release from first start (bundled Zig compile) to live play on each, one after the other
# (~10 min each on lavapipe). Packages once, from -BuildDir: a runtime built like GitHub builds it
# (its glibc floor is what players get): tools/build_linux_release.sh in an ubuntu:22.04 container into /root/blitz-r2204
# (CLAUDE.md, Linux).
#   powershell -ExecutionPolicy Bypass -File tools/verify_distros.ps1 [-Only "arch,fedora"] [-BuildDir /root/blitz-u2404]
param([string[]]$Only = @(), [string]$BuildDir = "/root/blitz-r2204", [int]$Seconds = 600, [int]$Every = 60, [switch]$Rebuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$all = 'arch', 'fedora', 'ubuntu2404', 'ubuntu2604', 'debian13', 'opensuse', 'ubuntu2204', 'debian12'
# -File passes "-Only arch,fedora" as one string.
$list = if ($Only) { @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) } else { $all }
$bad = @($list | Where-Object { $all -notcontains $_ })
if ($bad) { throw "unknown distribution(s): $($bad -join ', ') (one of: $($all -join ', '))" }

# One package for all of them; a failed packaging run must not leave an older archive to test.
$wslRoot = '/mnt/' + $root.Substring(0, 1).ToLower() + ($root.Substring(2) -replace '\\', '/')
& wsl.exe -u root -e sh -c "cd '$wslRoot' && BLITZ_MAX_GLIBC=2.35 sh tools/package_linux.sh '$BuildDir' /root/blitz-verify-pkg/NFLBlitz2003"
if ($LASTEXITCODE) { throw "packaging failed (exit $LASTEXITCODE)" }

$results = @()
foreach ($d in $list) {
  $a = @('-ExecutionPolicy', 'Bypass', '-File', "$root\tools\verify_linux.ps1", '-Image', $d, '-BuildDir', $BuildDir,
         '-Seconds', "$Seconds", '-Every', "$Every", '-SkipPackage')
  if ($Rebuild) { $a += '-Rebuild' }
  # The child's stderr lines must not become terminating errors here (that would end the whole list).
  $ErrorActionPreference = 'Continue'
  $o = & powershell @a 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  New-Item -ItemType Directory -Force "$root\build\verify_linux\$d" | Out-Null
  $o | Set-Content -Encoding utf8 "$root\build\verify_linux\$d\result.txt"
  $verdict = ($o | Where-Object { $_ -match '^(PASS|FAIL)' }) -join ' | '
  if (-not $verdict) { $verdict = "FAIL: no result (exit $code; $($o | Select-Object -Last 1))" }
  elseif ($code -and $verdict -notmatch 'FAIL') { $verdict = "FAIL: exit $code | $verdict" }
  $results += "{0,-11} {1}" -f $d, $verdict
  Write-Host $results[-1]
  $results | Set-Content -Encoding utf8 "$root\build\verify_linux\distros.txt"
}
""
$results
