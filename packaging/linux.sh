#!/bin/bash
# Build the Linux x64 package.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build/linux}
OUT=dist/linux/M365-BEC
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
rm -rf dist/linux
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/glib-2.0/schemas" "$OUT/share/icons/hicolor/256x256/apps"
cp "$BUILD/m365-bec" "$OUT/bin/m365-bec-bin"
# Bundle everything except glibc.
ldd "$BUILD/m365-bec" | awk '/=> \//{print $3}' |
    grep -Ev '/(libc|libm|libdl|libpthread|librt|libresolv|ld-linux[^/]*)\.so' |
    while read -r lib; do cp -L "$lib" "$OUT/lib/"; done
cp /usr/share/glib-2.0/schemas/gschemas.compiled "$OUT/share/glib-2.0/schemas/"
cp assets/classic-envelope.png "$OUT/share/icons/hicolor/256x256/apps/m365-bec.png"
cp LICENSE "$OUT/"
strip "$OUT/bin/m365-bec-bin" "$OUT"/lib/*.so*
cat > "$OUT/m365-bec" <<'EOF'
#!/bin/sh
# Run with bundled libraries.
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GSETTINGS_SCHEMA_DIR="$HERE/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$HERE/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/bin/m365-bec-bin" "$@"
EOF
chmod +x "$OUT/m365-bec"
cat > "$OUT/m365-bec.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=M365 Email Log Analyzer
Exec=m365-bec %F
Icon=m365-bec
Categories=Utility;Security;
EOF
tar -C dist/linux -cJf dist/linux/M365-BEC-linux-x64.tar.xz M365-BEC
du -sh "$OUT" dist/linux/M365-BEC-linux-x64.tar.xz
