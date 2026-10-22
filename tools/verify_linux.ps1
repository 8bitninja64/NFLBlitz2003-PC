# Linux release check, driven from Windows through WSL2 + WSLg (the NBA Hangtime port's
# verify_linux.py approach): the player path from the release archive to live play.
#   1. tools/package_linux.sh packs the Linux runtime (from -BuildDir, a WSL build directory) and
#      unpacks it into a fresh folder with an empty user directory (-Reuse keeps the last one, and
#      its compiled game module).
#   2. The first start compiles the game module from the disc (BLITZ_AUTO_BUILD), then plays the soak
#      input script (build/soak_input.txt: menus, team select, live play) for -Seconds.
#   3. The real WSLg window (an msrdc.exe RAIL window) is screenshotted every -Every seconds into
#      build/verify_linux/ (game content: local only, git-ignored), the audio is recorded (BLITZ_WAV).
# Checks: the module compiled and loaded; a real GPU adapter (not Aurora's Null fallback, which runs
# the game with sound and no picture); still running at the end; no FATAL/crash; a match started;
# frames keep being presented; the audio is audible (tools/wavstat.py).
#   powershell -ExecutionPolicy Bypass -File tools/verify_linux.ps1 [-Seconds 300] [-Every 30] [-Reuse]
# WSL GPU: Mesa's d3d12 driver (GALLIUM_DRIVER=d3d12, Dawn's OpenGL ES backend); on a laptop with two
# GPUs, -Gpu picks the D3D12 adapter by name (the integrated one fails with EGL_BAD_MATCH).
# -LinuxEnv 'K=V K2=V2' adds Linux environment, e.g. 'BLITZ_BACKEND=vulkan BLITZ_CPU_GPU=1' (Mesa's
# lavapipe: Vulkan on the CPU, slow but a second renderer to compare with).
param([int]$Seconds = 300, [int]$Every = 30, [string]$BuildDir = "/root/blitz-ci", [string]$Gpu = "NVIDIA",
      [string]$Disc = "NFL Blitz 2003 (USA).iso", [string]$Distro = "", [string]$LinuxEnv = "", [switch]$Reuse)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
function WslPath($p) { $f = [IO.Path]::GetFullPath($p); '/mnt/' + $f.Substring(0, 1).ToLower() + ($f.Substring(2) -replace '\\', '/') }
$out = Join-Path $root "build\verify_linux"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter "shot_*.png" | Remove-Item
$wslRoot = WslPath $root
$wslOut = WslPath $out
$wslDisc = WslPath $Disc
$wslArgs = @(); if ($Distro) { $wslArgs += @('-d', $Distro) }

# The Linux side: package, unpack, run (sh script: the input script is too long for a command line).
# -Reuse keeps the user directory (settings and the compiled game module), never the old runtime.
$fresh = if ($Reuse) { "rm -rf `"`$dir/NFLBlitz2003`"" } else { "rm -rf `"`$dir`"" }
$fresh += "; mkdir -p `"`$dir`"; tar -C `"`$dir`" -xJf `"`$pkg-linux-x64.tar.xz`""
@"
set -e
dir=/root/blitz-verify
pkg=/root/blitz-verify-pkg/NFLBlitz2003
cd '$wslRoot'
sh tools/package_linux.sh '$BuildDir' "`$pkg"
$fresh
mkdir -p "`$dir/home"
cd "`$dir"
export HOME="`$dir/home" XDG_DATA_HOME="`$dir/home/.local/share"
export GALLIUM_DRIVER=d3d12 LD_LIBRARY_PATH=/usr/lib/wsl/lib MESA_D3D12_DEFAULT_ADAPTER_NAME='$Gpu'
export BLITZ_AUTO_BUILD=1 BLITZ_WAV='$wslOut/run.wav'
export BLITZ_INPUT="`$(cat '$wslRoot/build/soak_input.txt')"
$(if ($LinuxEnv) { "export $LinuxEnv" })
set +e
timeout $Seconds ./NFLBlitz2003/NFLBlitz2003 --disc '$wslDisc' > '$wslOut/run.log' 2>&1
echo "exit=`$?" > '$wslOut/exit.txt'
"@ -replace "`r", "" | Set-Content -NoNewline -Encoding ascii "$out\run.sh"
Remove-Item "$out\exit.txt", "$out\run.log", "$out\run.wav" -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class WG { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  // Only WSLg windows (RAIL, hosted by msrdc.exe): the Windows build has the same title.
  public static IntPtr Find(string prefix) { IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => { var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
      if (IsWindowVisible(h) && sb.ToString().StartsWith(prefix)) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        try { if (System.Diagnostics.Process.GetProcessById((int)pid).ProcessName == "msrdc") { found = h; return false; } } catch { }
      } return true; }, IntPtr.Zero);
    return found; } }
"@
[void][WG]::SetProcessDPIAware()

$p = Start-Process -FilePath 'wsl.exe' -ArgumentList ($wslArgs + @('-u', 'root', '-e', 'sh', "$wslOut/run.sh")) -PassThru -WindowStyle Hidden
# The clock starts with the game window (packaging and the first-start compile come first).
$deadline = (Get-Date).AddSeconds(900)
while (-not $p.HasExited -and [WG]::Find("NFL Blitz 2003") -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
$start = Get-Date
"window after $([int]($start - $p.StartTime).TotalSeconds) s"
for ($t = $Every; $t -le $Seconds - 5; $t += $Every) {
  $wait = $t - ((Get-Date) - $start).TotalSeconds; if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  if ($p.HasExited) { break }
  $h = [WG]::Find("NFL Blitz 2003")
  if ($h -eq [IntPtr]::Zero) { "shot ${t}s: no window"; continue }
  $r = New-Object WG+RECT; [void][WG]::GetWindowRect($h, [ref]$r); $w = $r.R - $r.L; $hh = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $hh; $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][WG]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
  $file = Join-Path $out ("shot_{0:D4}s.png" -f $t)
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
  "shot ${t}s ${w}x${hh}"
}
[void]$p.WaitForExit(($Seconds + 120) * 1000)

# Checks.
$log = if (Test-Path "$out\run.log") { Get-Content -Raw "$out\run.log" } else { "" }
$fails = @()
$exit = if (Test-Path "$out\exit.txt") { (Get-Content "$out\exit.txt").Trim() } else { "exit=none" }
if ($exit -ne "exit=124") { $fails += "the game ended before the time was up ($exit; 124 = still running)" }
if ($log -notmatch 'game module .*: \d+ functions') { $fails += "the game module did not load" }
$device = [regex]::Match($log, 'Device: (.*)').Groups[1].Value.Trim()
if (-not $device -or $device -match 'Null') { $fails += "no GPU adapter (device: '$device'): the game ran without a picture" }
if ($log -match ' FATAL\]|Segmentation fault|Aborted') { $fails += "fatal error / crash in the log" }
# The attract demo is a match too: the players' match is the one the setup hook reports as such.
$matchOn = [regex]::Match($log, "match setup: players' match[\s\S]*?match on at frame (\d+)")
if (-not $matchOn.Success) { $fails += "no players' match started (scripted input did not reach live play)" }
$frames = [regex]::Matches($log, 'presented (\d+) frames') | ForEach-Object { [int]$_.Groups[1].Value } | Select-Object -Last 1
if (-not $frames) { $fails += "no frames presented" }
$wav = ""
if (Test-Path "$out\run.wav") {
  $py = (Get-Command py.exe -ErrorAction SilentlyContinue) ; $pyArgs = if ($py) { @('-3', '-I') } else { @('-I') }
  $wav = (& ($(if ($py) { $py.Source } else { 'python' })) @pyArgs tools/wavlevel.py "$out\run.wav")
  if ($LASTEXITCODE) { $fails += "the audio is mostly silent ($wav)" }
} else { $fails += "no audio recording" }
"device: $device"
"players' match: $(if ($matchOn.Success) { 'kicked off at frame ' + $matchOn.Groups[1].Value } else { 'none' }); frames presented: $frames"
"audio: $wav"
"screenshots, log and audio: $out"
if ($fails) { $fails | ForEach-Object { "FAIL: $_" }; exit 1 }
"PASS: Linux release under WSLg: first-start compile, $device, live play, audio"
