# Runs the game for a while, screenshots its window, then closes it.
#   powershell -File tools/capture_window.ps1 [-Seconds 15] [-Out build/shot.png] [-ExeArgs '...']
param(
  [int]$Seconds = 15,
  [string]$Out = "build/shot.png",
  [string]$Exe = "build/windows-clang/NFLBlitz2003.exe",
  [string]$ExeArgs = "",
  [string]$Log = "build/run_window.log"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Name Dpi -Namespace Win32 -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[Win32.Dpi]::SetProcessDPIAware() | Out-Null   # capture physical pixels on scaled displays
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$psi = @{ FilePath = $Exe; PassThru = $true; RedirectStandardError = $Log; RedirectStandardOutput = "$Log.out" }
if ($ExeArgs) { $psi.ArgumentList = $ExeArgs }
$env:BLITZ_TOPMOST = '1'
$p = Start-Process @psi
Start-Sleep -Seconds $Seconds
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; if (-not $p.HasExited) { $p.Kill() }; exit 1 }
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
# Swapchain content isn't visible to PrintWindow: bring the window up and copy the screen.
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $root $Out), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
if (-not $p.HasExited) { $p.Kill() }
Write-Output "saved $Out ($w x $hgt)"
