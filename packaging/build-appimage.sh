#!/bin/sh
# Build Drive2Iso and package the Linux binary as an AppImage.
# Requires: a C toolchain (make/gcc) and `appimagetool` on PATH.
set -e
cd "$(dirname "$0")/.."

make
install -Dm755 drive2iso packaging/AppDir/usr/bin/drive2iso
chmod +x packaging/AppDir/AppRun

# appimagetool reads AppDir/*.desktop + the matching icon at the AppDir root.
appimagetool packaging/AppDir "Drive2Iso-x86_64.AppImage"
echo "built Drive2Iso-x86_64.AppImage"
