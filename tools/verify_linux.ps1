# Linux release check, driven from Windows through WSL2 + WSLg (the NBA Hangtime port's
# verify_linux.py approach): the player path from the release archive to live play.
#   1. tools/package_linux.sh packs the Linux runtime (from -BuildDir, a WSL build directory) and
#      unpacks it into a fresh folder with an empty user directory (-Reuse keeps the last one, and
#      its compiled game module).
#   2. The first start compiles the game module from the disc (BLITZ_AUTO_BUILD), then plays the soak
#      input script (tools/verify_input.txt: menu presses until the players' match starts, then the soak script's gameplay input) for -Seconds.
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
# -Image <distro> runs the game inside a Podman container of that distribution instead (WSLg's X11
# and PulseAudio sockets mounted; Mesa's lavapipe Vulkan): only what a desktop install has, no compiler
# (the release's bundled Zig must do the first-start build). The image is made once
# (localhost/blitz-test-<distro>, -Rebuild remakes it); outputs go to build/verify_linux/<distro>/.
# tools/verify_distros.ps1 runs the whole list.
param([int]$Seconds = 300, [int]$Every = 30, [string]$BuildDir = "/root/blitz-ci", [string]$Gpu = "NVIDIA",
      [string]$Disc = "NFL Blitz 2003 (USA).iso", [string]$Distro = "", [string]$LinuxEnv = "", [switch]$Reuse,
      [string]$Image = "", [switch]$Rebuild, [switch]$SkipPackage)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
# The disc: a given relative path is the caller's, the default name the repository root's.
$Disc = if ($PSBoundParameters.ContainsKey('Disc')) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Disc) } else { Join-Path $root $Disc }
if (-not (Test-Path -LiteralPath $Disc)) { throw "disc image not found: $Disc" }
Set-Location $root
function WslPath($p) { $f = [IO.Path]::GetFullPath($p); '/mnt/' + $f.Substring(0, 1).ToLower() + ($f.Substring(2) -replace '\\', '/') }

# Distributions: base image and the packages a desktop install would have (runtime libraries the game
# and SDL load, Mesa Vulkan, python3 for the first-start build; never a compiler).
$apt = 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y -qq python3 xz-utils mesa-vulkan-drivers libvulkan1 libx11-6 libxext6 libxcursor1 libxi6 libxrandr2 libxfixes3 libxss1 libxkbcommon0 libpulse0 libsqlite3-0 libzstd1'
$Distros = [ordered]@{
  'arch'       = @('docker.io/library/archlinux:latest', 'pacman -Syu --noconfirm --needed python xz tar vulkan-swrast vulkan-icd-loader libx11 libxext libxcursor libxi libxrandr libxfixes libxss libxkbcommon libpulse sqlite zstd')
  'fedora'     = @('registry.fedoraproject.org/fedora:latest', 'dnf -y install python3 xz tar mesa-vulkan-drivers vulkan-loader libX11 libXext libXcursor libXi libXrandr libXfixes libXScrnSaver libxkbcommon pulseaudio-libs sqlite-libs libzstd libstdc++')
  'ubuntu2404' = @('docker.io/library/ubuntu:24.04', $apt)
  'ubuntu2604' = @('docker.io/library/ubuntu:26.04', $apt)
  'debian13'   = @('docker.io/library/debian:trixie', $apt)
  'opensuse'   = @('registry.opensuse.org/opensuse/tumbleweed:latest', 'zypper -n in python3 xz tar gzip libvulkan_lvp libvulkan1 libX11-6 libXext6 libXcursor1 libXi6 libXrandr2 libXfixes3 libXss1 libxkbcommon0 libpulse0 libsqlite3-0 libzstd1')
  'ubuntu2204' = @('docker.io/library/ubuntu:22.04', $apt)
  'debian12'   = @('docker.io/library/debian:bookworm', $apt)
}
if ($Image -and -not $Distros.Contains($Image)) { throw "unknown -Image $Image (one of: $($Distros.Keys -join ', '))" }

$out = Join-Path $root "build\verify_linux"
if ($Image) { $out = Join-Path $out $Image }
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter "shot_*.png" | Remove-Item
$wslRoot = WslPath $root
$wslOut = WslPath $out
$wslDisc = WslPath $Disc
$wslArgs = @(); if ($Distro) { $wslArgs += @('-d', $Distro) }
$dir = if ($Image) { "/root/blitz-verify-$Image" } else { "/root/blitz-verify" }

# The game's start (game.sh, run on the WSL host or inside the container; the input script is too long
# for a command line). Host: Mesa's d3d12 driver; container: lavapipe.
$gpuEnv = if ($Image) { "export BLITZ_BACKEND=vulkan BLITZ_CPU_GPU=1" } else { "export GALLIUM_DRIVER=d3d12 LD_LIBRARY_PATH=/usr/lib/wsl/lib MESA_D3D12_DEFAULT_ADAPTER_NAME='$Gpu'" }
@"
cd '$dir'
export HOME='$dir/home' XDG_DATA_HOME='$dir/home/.local/share'
$gpuEnv
export BLITZ_AUTO_BUILD=1 BLITZ_WAV='$wslOut/run.wav'
export BLITZ_INPUT="`$(cat '$dir/soak_input.txt')"
$(if ($LinuxEnv) { "export $LinuxEnv" })
timeout $Seconds ./NFLBlitz2003/NFLBlitz2003 --disc '$wslDisc' > '$wslOut/run.log' 2>&1
echo "exit=`$?" > '$wslOut/exit.txt'
"@ -replace "`r", "" | Set-Content -NoNewline -Encoding ascii "$out\game.sh"

# The Linux side: package, unpack, (make the image,) run.
# -Reuse keeps the user directory (settings and the compiled game module), never the old runtime.
$fresh = if ($Reuse) { "rm -rf '$dir/NFLBlitz2003'" } else { "rm -rf '$dir'" }
$fresh += "; mkdir -p '$dir/home'; tar -C '$dir' -xJf `"`$pkg-linux-x64.tar.xz`""
$launch = "sh '$wslOut/game.sh'"
if ($Image) {
  $img = "localhost/blitz-test-$Image"
  $setup = $Distros[$Image][1] -replace "'", "'\''"
  $launch = @"
if [ -n '$(if ($Rebuild) { 1 })' ] || ! podman image exists $img; then
  podman rm -f blitz-setup-$Image >/dev/null 2>&1 || true
  podman run --name blitz-setup-$Image $($Distros[$Image][0]) sh -c '$setup' > '$wslOut/image.log' 2>&1 || { echo "image setup failed" > '$wslOut/exit.txt'; exit 1; }
  podman commit -q blitz-setup-$Image $img >/dev/null && podman rm blitz-setup-$Image >/dev/null
fi
podman run --rm -e DISPLAY=:0 -v /mnt/wslg/.X11-unix:/tmp/.X11-unix -v /mnt/wslg:/mnt/wslg \
  -e PULSE_SERVER=unix:/mnt/wslg/PulseServer -v '${dir}:${dir}' -v '${wslOut}:${wslOut}' -v '${wslDisc}:${wslDisc}:ro' \
  $img sh '$wslOut/game.sh'
"@
}
@"
set -e
pkg=/root/blitz-verify-pkg/NFLBlitz2003
cd '$wslRoot'
$(if (-not $SkipPackage) { "sh tools/package_linux.sh '$BuildDir' `"`$pkg`"" })
$fresh
cp '$wslRoot/tools/verify_input.txt' '$dir/soak_input.txt'
set +e
$launch
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
# The clock starts with the first window: on a fresh run the first-start build window (packaging and
# image setup come first); the game's own timeout includes the build.
$deadline = (Get-Date).AddSeconds(2400)   # image setup, packaging, first-start compile
while (-not $p.HasExited -and [WG]::Find("NFL Blitz 2003") -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
$start = Get-Date
"first window after $([int]($start - $p.StartTime).TotalSeconds) s"
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
# A real audio device (BLITZ_WAV records without one).
$audio = [regex]::Match($log, 'audio: (\S+) driver, device open').Groups[1].Value
if (-not $audio) { $fails += "no audio device opened" }
# The first-start build (absent under -Reuse when the module is current) must use the bundled Zig in
# the containers, which have no compiler.
$built = $log -match 'compiling the game code'
$compiler = [regex]::Match($log, 'compiler: (.*)').Groups[1].Value.Trim()
if ($Image -and $built -and $compiler -notmatch 'zig') { $fails += "the first-start build did not use the bundled Zig ('$compiler')" }
if ($Image -and -not $built -and -not $Reuse) { $fails += "no first-start build in a fresh user directory" }
"$(if ($Image) { "$Image " })device: $device; compiler: $compiler"
"players' match: $(if ($matchOn.Success) { 'kicked off at frame ' + $matchOn.Groups[1].Value } else { 'none' }); frames presented: $frames"
"audio: $audio; $wav"
"screenshots, log and audio: $out"
if ($fails) { $fails | ForEach-Object { "FAIL: $_" }; exit 1 }
"PASS: Linux release$(if ($Image) { " on $Image" }) under WSLg: first-start compile ($compiler), $device, live play, audio"
