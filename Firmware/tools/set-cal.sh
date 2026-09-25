#!/usr/bin/env bash
# Zet calibration.txt betrouwbaar op de EMISNIFFER-schijf en flusht naar de
# flash van de unit (schone SCSI-eject). Omzeilt het "busy"-eject-gedoe op
# Linux-desktops.
#
# Gebruik:  ./tools/set-cal.sh <adc_min> <adc_max> [smoothing] [vbat_cal]
#   bv:     ./tools/set-cal.sh 200 2000
#           ./tools/set-cal.sh 200 2000 85 1.17

set -euo pipefail
export MTOOLS_SKIP_CHECK=1

adc_min="${1:?adc_min ontbreekt}"
adc_max="${2:?adc_max ontbreekt}"
smoothing="${3:-85}"
vbat_cal="${4:-1.17}"

# FatFS-schijf van de unit vinden (klein vfat, USB)
dev="$(lsblk -rno NAME,SIZE,FSTYPE,TRAN 2>/dev/null \
       | awk '$3=="vfat" && $4=="usb" && $2 ~ /K$/ {print "/dev/"$1; exit}')"
[ -n "$dev" ] || { echo "!! Geen EMISNIFFER-schijf gevonden (unit ingeplugd?)"; exit 1; }
echo ">> Schijf: $dev"

# Loskoppelen (houders killen), zodat mtools rauw kan schrijven
pkill -x nautilus 2>/dev/null || true
sleep 0.5
fuser -km "$dev" 2>/dev/null || true
sleep 0.5
udisksctl unmount -b "$dev" >/dev/null 2>&1 || umount "$dev" 2>/dev/null || true

# calibration.txt schrijven
tmp="$(mktemp)"
printf '# VU-meter + batterij kalibratie\r\n# adc_min->0 segm, adc_max->8 segm, smoothing 0..95, vbat_cal = gemeten/afgelezen\r\n\r\nadc_min=%s\r\nadc_max=%s\r\nsmoothing=%s\r\nvbat_cal=%s\r\n' \
  "$adc_min" "$adc_max" "$smoothing" "$vbat_cal" > "$tmp"
mcopy -i "$dev" -o "$tmp" ::calibration.txt
rm -f "$tmp"

# Echte SCSI-eject -> firmware flusht naar flash + herlaadt
eject "$dev"
echo ">> Klaar: adc_min=$adc_min adc_max=$adc_max smoothing=$smoothing vbat_cal=$vbat_cal"
echo "   (de unit herlaadt de kalibratie nu; steek de schijf evt. opnieuw in om te bewerken)"
