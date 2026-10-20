# Imports the Visual Studio x64 developer environment into the current PowerShell session.
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") + ";" + $env:PATH
