# Bundle the Drive2Iso GUI into a standalone Windows .exe with PyInstaller.
#
# Prereq (MSYS2):  pacman -S mingw-w64-x86_64-pyinstaller
# Output:          gui/dist/Drive2Iso-GUI.exe  (one file, ~12 MB, self-contained)
$ErrorActionPreference = "Stop"

# Prefer the MSYS2 mingw Python (Tk-capable); fall back to python on PATH.
$py = "C:\msys64\mingw64\bin\python.exe"
if (-not (Test-Path $py)) { $py = (Get-Command python).Source }

$root = Split-Path -Parent $PSScriptRoot   # repo root (this script lives in gui/)
Set-Location $root

& $py -m PyInstaller --onefile --windowed --name Drive2Iso-GUI `
    --distpath gui/dist --workpath gui/build --specpath gui --noconfirm `
    gui/drive2iso_gui.py

Write-Host "built gui/dist/Drive2Iso-GUI.exe"
