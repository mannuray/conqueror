#!/usr/bin/env bash
# Build a bridge release from a clean, tagged tree and publish it on GitHub (design-notes §27).
#   git tag bridge-v0.1.0 && scripts/make_release.sh bridge-v0.1.0
set -euo pipefail
cd "$(dirname "$0")/.."
TAG="${1:?usage: scripts/make_release.sh bridge-vX.Y.Z}"
[[ "$TAG" == bridge-v* ]] || { echo "tag must start with bridge-v"; exit 1; }
[ "$(git describe --tags --exact-match HEAD 2>/dev/null)" = "$TAG" ] || { echo "HEAD is not tagged $TAG"; exit 1; }
pio run -e bridge_release   # refuses a dirty tree; also writes build/source.bin
OUT="dist/$TAG"
rm -rf "$OUT" && mkdir -p "$OUT"
B=.pio/build/bridge_release
cp "$B/bootloader.bin" "$B/partitions.bin" "$B/firmware.bin" build/source.bin "$OUT/"
cp "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$OUT/"
cat > "$OUT/flash.sh" <<'FLASH'
#!/usr/bin/env bash
# Flash this release over USB: ./flash.sh /dev/ttyACM0   (needs: uv tool install esptool)
set -euo pipefail
PORT="${1:?usage: ./flash.sh /dev/ttyACM0}"
cd "$(dirname "$0")"
esptool --chip esp32s3 --port "$PORT" flash_id | grep -Eq "Chip (is|type:) *ESP32-S3" || { echo "not an ESP32-S3 - refusing"; exit 1; }
esptool --chip esp32s3 --port "$PORT" --baud 921600 write_flash --flash_mode dio --flash_size 8MB \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin 0x610000 source.bin
FLASH
chmod +x "$OUT/flash.sh"
gh release create "$TAG" "$OUT"/* --title "$TAG" \
  --notes "ESP32-S3 developer bridge $TAG (commit $(git rev-parse --short HEAD)). DEVELOPER BRIDGE - NO AUTHENTICATION - home network only. Flash: ./flash.sh /dev/ttyACM0"
echo "released $TAG"
