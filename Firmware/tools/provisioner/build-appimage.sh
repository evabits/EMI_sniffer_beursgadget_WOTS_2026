#!/usr/bin/env bash
# Bouwt EMIsniffer-Provisioner-x86_64.AppImage reproduceerbaar.
# Bundelt een relocatable Python (mét tkinter) + mtools + de provisioning-app.
# Vereist: internet (download appimagetool + python-build-standalone), x86_64 Linux.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260825/cpython-3.12.14%2B20260825-x86_64-unknown-linux-gnu-install_only.tar.gz"
AIT_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
OUT="$HERE/EMIsniffer-Provisioner-x86_64.AppImage"

echo ">> Downloaden..."
wget -q "$AIT_URL" -O "$WORK/appimagetool"; chmod +x "$WORK/appimagetool"
wget -q "$PY_URL" -O "$WORK/python.tar.gz"
tar xzf "$WORK/python.tar.gz" -C "$WORK"   # -> $WORK/python

echo ">> AppDir samenstellen..."
APP="$WORK/AppDir"
mkdir -p "$APP/usr/bin" "$APP/usr/lib"
cp -r "$WORK/python" "$APP/usr/python"
for b in mcopy mlabel minfo mdir; do cp "$(command -v "$b")" "$APP/usr/bin/"; done
cp "$HERE/emisniffer_provisioner.py" "$APP/usr/lib/"

cat > "$APP/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="$HERE"
export PATH="$HERE/usr/bin:$PATH"
exec "$HERE/usr/python/bin/python3" "$HERE/usr/lib/emisniffer_provisioner.py" "$@"
EOF
chmod +x "$APP/AppRun"

cat > "$APP/emisniffer-provisioner.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=EMIsniffer Provisioner
Exec=AppRun
Icon=emisniffer-provisioner
Categories=Utility;
Terminal=false
EOF

"$APP/usr/python/bin/python3" - "$APP/emisniffer-provisioner.png" <<'PY'
import zlib, struct, sys
w=h=256; rgb=(26,127,55)
raw=b''.join(b'\x00'+bytes(rgb)*w for _ in range(h))
def ch(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'
    +ch(b'IHDR',struct.pack(">IIBBBBB",w,h,8,2,0,0,0))
    +ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b''))
PY
cp "$APP/emisniffer-provisioner.png" "$APP/.DirIcon"

echo ">> AppImage bouwen..."
( cd "$WORK" && ARCH=x86_64 ./appimagetool --appimage-extract-and-run AppDir "$OUT" )

rm -rf "$WORK"
echo ">> Klaar: $OUT"
