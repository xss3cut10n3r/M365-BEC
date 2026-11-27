#!/bin/bash
# Build the Windows x64 package.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH=/c/msys64/ucrt64/bin:$PATH
ROOT=/c/msys64/ucrt64
OUT=dist/windows/M365-BEC
cmake -S . -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win
rm -rf dist/windows
mkdir -p "$OUT/share/glib-2.0/schemas"
cp build/win/M365-BEC.exe "$OUT/"
# Copy only linked libraries.
ntldd -R build/win/M365-BEC.exe | grep -i 'ucrt64' | sed 's/.*=> \(.*\) (0x.*/\1/' | tr '\\' '/' | sort -u |
    while read -r dll; do cp "$(cygpath -u "$dll")" "$OUT/"; done
cp "$ROOT/share/glib-2.0/schemas/gschemas.compiled" "$OUT/share/glib-2.0/schemas/"
cp LICENSE "$OUT/"
strip "$OUT"/*.exe "$OUT"/*.dll
(cd dist/windows && rm -f M365-BEC-windows-x64.zip && tar.exe -a -c -f M365-BEC-windows-x64.zip M365-BEC)
du -sh "$OUT" dist/windows/M365-BEC-windows-x64.zip
