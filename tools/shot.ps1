# Runs the game on the soak input script (build/soak_input.txt) until frame shot -Frame exists, copies it
# (and its alpha image) to -Out and stops the game. -Env sets extra variables ("NAME=value|NAME=value";
# values may contain ;). Run it as its own process so the variables do not stay in your shell:
#   powershell -ExecutionPolicy Bypass -File tools/shot.ps1 -Frame 2700 -Out build/x.png [-Env "BLITZ_GX_PAINT=n307"]
param([int]$Frame = 2700, [string]$Out, [string]$Env = "", [int]$Timeout = 200)
$root = Split-Path -Parent $PSScriptRoot
$shot = Join-Path $root "build\shots\frame_$Frame.png"
$alpha = Join-Path $root "build\shots\frame_${Frame}_alpha.png"
$env:BLITZ_INPUT = (Get-Content (Join-Path $root 'build\soak_input.txt') -Raw).Trim()
$env:BLITZ_MUTE = '1'
$env:BLITZ_SET = 'msaa=1'
$env:BLITZ_SHOT = "$Frame"
foreach ($kv in ($Env -split '\|')) {
  if ($kv -match '^([^=]+)=(.*)$') { Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] }
}
Remove-Item -LiteralPath $shot, $alpha -ErrorAction SilentlyContinue
$p = Start-Process (Join-Path $root 'build\windows-clang\NFLBlitz2003.exe') -WorkingDirectory $root -PassThru `
  -RedirectStandardError (Join-Path $root 'build\shot_err.log') -RedirectStandardOutput (Join-Path $root 'build\shot_out.log')
$t = 0
while (-not (Test-Path $shot) -and $t -lt $Timeout -and -not $p.HasExited) { Start-Sleep 1; $t++ }
Start-Sleep 2
if (-not $p.HasExited) { Stop-Process $p -Force }
if (-not (Test-Path $shot)) { Write-Error "no shot of frame $Frame after $t s"; exit 1 }
if ($Out) {
  if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
  Copy-Item $shot $Out
  if (Test-Path $alpha) {
    $dir = Split-Path -Parent $Out
    Copy-Item $alpha (Join-Path $dir ([System.IO.Path]::GetFileNameWithoutExtension($Out) + '_alpha.png'))
  }
}
"frame $Frame after $t s"
