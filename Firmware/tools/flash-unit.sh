#!/usr/bin/env bash
# Flash een EMIsniffer-unit via USB (geen J-Link). Robuust voor zowel blanco
# units (staan vanzelf in BOOTSEL) als AL DRAAIENDE units (worden via een
# 1200bps-touch op de juiste CDC-poort naar BOOTSEL gereset).
#
# Waarom niet gewoon `pio run -t upload`: op een draaiende unit kan pio de
# 1200bps-reset op een STALE /dev/ttyACM* doen (geen BOOTSEL) en vervolgens de
# UF2 op de FatFS-DATASCHIJF dumpen i.p.v. te flashen. Dit script pakt de juiste
# poort en kopieert expliciet naar de BOOTSEL-schijf.

set -euo pipefail

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
UF2="$PROJECT_DIR/.pio/build/rp2354/firmware.uf2"
VID="2e8a"; PID_RUN="f00f"; PID_BOOT="000f"

# --- USB-helpers (sysfs) -----------------------------------------------------
# Loop vanaf een sysfs-startpad omhoog tot idVendor/idProduct; echo "vid pid".
_usb_ids() {
  local d; d="$(readlink -f "$1" 2>/dev/null || true)"
  for _ in $(seq 1 12); do
    if [ -f "$d/idVendor" ] && [ -f "$d/idProduct" ]; then
      echo "$(cat "$d/idVendor") $(cat "$d/idProduct")"; return 0
    fi
    [ "$d" = "/" ] && break; d="$(dirname "$d")"
  done
  return 1
}

# CDC-seriële poort van een draaiende unit (VID:PID_RUN).
find_cdc_port() {
  local tty n ids
  for tty in /dev/ttyACM*; do
    [ -e "$tty" ] || continue
    n="$(basename "$tty")"
    ids="$(_usb_ids "/sys/class/tty/$n/device" || true)"
    [ "$ids" = "$VID $PID_RUN" ] && { echo "$tty"; return 0; }
  done
  return 1
}

# Mountpoint van de BOOTSEL-schijf. Detecteert op het FAT-label "RP2350" (uniek
# voor de BOOTSEL-schijf; niet de FatFS-dataschijf) en mount 'm zo nodig zelf.
# Werkt ook als de schijf een partitie heeft (sda1) i.p.v. superfloppy.
bootsel_mount() {
  local mp dev
  mp="$(findmnt -rno TARGET -S LABEL=RP2350 2>/dev/null | head -1 || true)"
  [ -n "$mp" ] && { echo "$mp"; return 0; }
  dev="$(blkid -L RP2350 2>/dev/null || true)"   # kan root vereisen op headless
  [ -n "$dev" ] || return 1
  udisksctl mount -b "$dev" >/dev/null 2>&1 || true
  mp="$(findmnt -rno TARGET -S LABEL=RP2350 2>/dev/null | head -1 || true)"
  [ -n "$mp" ] && { echo "$mp"; return 0; }
  return 1
}

# 1200bps-touch (reset naar BOOTSEL) op een CDC-poort.
touch_1200() {
  python3 - "$1" <<'PY'
import termios, os, sys, time, fcntl, array
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd); a[4] = termios.B1200; a[5] = termios.B1200
termios.tcsetattr(fd, termios.TCSANOW, a)
# Expliciete DTR val-flank @1200 baud = de reset-trigger (een kale close is
# onbetrouwbaar). TIOCM_DTR = 0x002.
buf = array.array('i', [0x002])
fcntl.ioctl(fd, termios.TIOCMBIS, buf); time.sleep(0.05)   # DTR aan
fcntl.ioctl(fd, termios.TIOCMBIC, buf); time.sleep(0.05)   # DTR uit -> reset
os.close(fd)
PY
}

# --- Flashen -----------------------------------------------------------------
echo ">> Firmware bouwen..."
"$PIO" run >/dev/null
[ -f "$UF2" ] || { echo "!! $UF2 ontbreekt"; exit 1; }

echo ">> Unit zoeken..."
mp="$(bootsel_mount || true)"
if [ -n "$mp" ]; then
  echo "   blanco/BOOTSEL-unit gevonden: $mp"
else
  cdc="$(find_cdc_port || true)"
  [ -n "$cdc" ] || { echo "!! Geen unit gevonden (BOOTSEL noch draaiend $VID:$PID_RUN)."; exit 1; }
  echo "   draaiende unit op $cdc -> 1200bps-reset naar BOOTSEL..."
  touch_1200 "$cdc"
  for _ in $(seq 1 20); do
    mp="$(bootsel_mount || true)"; [ -n "$mp" ] && break
    sleep 1
  done
  [ -n "$mp" ] || { echo "!! BOOTSEL-schijf verscheen niet na de reset."; exit 1; }
  echo "   BOOTSEL: $mp"
fi

echo ">> UF2 kopieren naar $mp ..."
cp "$UF2" "$mp/"
sync

echo ">> Wachten tot de unit boot..."
for _ in $(seq 1 20); do
  lsusb | grep -qi "$VID:$PID_RUN" && { echo ">> Klaar. Unit geflasht en draait."; exit 0; }
  sleep 1
done
echo ">> UF2 gekopieerd; unit lijkt nog niet terug (check handmatig)."
