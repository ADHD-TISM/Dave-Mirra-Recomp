# Create a desktop shortcut to dmrecomp.exe, starting in the exe's own folder
# (that is where dmrecomp.ini and the memcard*.mcd saves live).
# usage: powershell -File make_shortcut.ps1 <path to dmrecomp.exe> [-Force]
param([Parameter(Mandatory=$true)][string]$Exe, [switch]$Force)
$exe = (Resolve-Path -LiteralPath $Exe).Path
$lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Dave Mirra Freestyle BMX (recomp).lnk'
if ((Test-Path -LiteralPath $lnk) -and -not $Force) { Write-Output "shortcut exists, left alone: $lnk"; exit 0 }
$s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = Split-Path -Parent $exe
$s.Description = 'Dave Mirra Freestyle BMX (static recompilation)'
$s.Save()
Write-Output "shortcut written: $lnk"
