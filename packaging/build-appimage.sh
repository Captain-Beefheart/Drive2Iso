#!/bin/sh
# Build Drive2Iso and package the Linux binary as an AppImage.
# Uses appimagetool from PATH, or downloads it if missing (so this works both on
# a dev box and in CI). Requires a C toolchain (make/gcc) and wget.
set -e
cd "$(dirname "$0")/.."

make
install -Dm755 drive2iso packaging/AppDir/usr/bin/drive2iso
chmod +x packaging/AppDir/AppRun

APPIMAGETOOL="$(command -v appimagetool || true)"
if [ -z "$APPIMAGETOOL" ]; then
    echo "appimagetool not on PATH; downloading the continuous build..."
    wget -q https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage \
        -O /tmp/appimagetool
    chmod +x /tmp/appimagetool
    APPIMAGETOOL=/tmp/appimagetool
fi

# APPIMAGE_EXTRACT_AND_RUN avoids needing FUSE (e.g. on CI runners).
ARCH="${ARCH:-x86_64}" APPIMAGE_EXTRACT_AND_RUN=1 \
    "$APPIMAGETOOL" packaging/AppDir "Drive2Iso-x86_64.AppImage"
echo "built Drive2Iso-x86_64.AppImage"
