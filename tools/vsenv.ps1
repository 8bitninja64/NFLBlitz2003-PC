# Imports a Visual Studio 2022 or newer x64 developer environment into this PowerShell session.
$vs = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -version "[17.0,)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) {
  # VS 2026 uses the major version (18) for its default installation directory.
  foreach ($version in "18", "2022") {
    foreach ($edition in "BuildTools", "Community", "Professional", "Enterprise") {
      foreach ($base in "${env:ProgramFiles(x86)}", "$env:ProgramFiles") {
        $p = "$base\Microsoft Visual Studio\$version\$edition"
        if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { $vs = $p; break }
      }
      if ($vs) { break }
    }
    if ($vs) { break }
  }
}
if (-not $vs) { throw "Visual Studio 2022 or newer (or its Build Tools) with the C++ workload was not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") + ";" + $env:PATH
