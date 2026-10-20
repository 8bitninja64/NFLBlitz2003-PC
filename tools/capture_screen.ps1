# Screenshot of the primary screen: powershell -File tools/capture_screen.ps1 -Out build/screen.png
param([string]$Out = "build/screen.png", [double]$Scale = 0.5)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($b.Width * $Scale)), ([int]($b.Height * $Scale))
$small.Save((Join-Path (Split-Path -Parent $PSScriptRoot) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Out"
