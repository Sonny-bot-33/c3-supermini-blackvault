#!/usr/bin/env bash
set -euo pipefail
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  echo "Uso: ./publish_ota.sh 0.1.2"
  exit 1
fi
TAG="v$VERSION"
BIN_PATH=".pio/build/esp32-c3-devkitm-1/firmware.bin"
if [ ! -f "$BIN_PATH" ]; then
  echo "No existe $BIN_PATH"
  echo "Compila primero con: ~/.platformio/penv/bin/pio run -e esp32-c3-devkitm-1"
  exit 1
fi
cp "$BIN_PATH" firmware.bin
python3 - <<PY
from pathlib import Path
p = Path('manifest.json')
p.write_text('''{\n  "version": "%s",\n  "firmwareUrl": "https://github.com/Sonny-bot-33/c3-supermini-blackvault/releases/download/%s/firmware.bin"\n}\n''' % ("$VERSION", "$TAG"), encoding='utf-8')
PY
git add manifest.json firmware.bin
if ! git diff --cached --quiet; then
  git commit -m "Release $TAG"
fi
git push
if gh release view "$TAG" >/dev/null 2>&1; then
  gh release upload "$TAG" firmware.bin --clobber
else
  gh release create "$TAG" firmware.bin --title "$TAG" --notes "OTA firmware $TAG"
fi
echo "Publicado $TAG"
