# EMIsniffer provisioning-station

Aparte provisioningstap (los van het flashen). Bewaakt USB en zodra een
**geflashte, draaiende** EMIsniffer-unit wordt ingeplugd, zet het automatisch:

- het volumelabel op **EMISNIFFER**, en
- een standaard **`calibration.txt`** (adc_min=730, adc_max=4095, smoothing=85).

Een GUI-venster toont per unit groot **KLAAR ✓** (groen) of **FOUT ✗** (rood) + log.

## Gebruik

```bash
sudo ./EMIsniffer-Provisioner-x86_64.AppImage
```

Draai als **root**: het label zetten schrijft naar het rauwe block-device.
Daarna: plug een geflashte unit in → het wordt vanzelf geprovisioneerd →
"KLAAR ✓ — unit loskoppelen" → volgende unit.

De app unmount de schijf automatisch vóór het schrijven (mtools werkt op het
rauwe device, dat botst met een gemounte FS). Op een desktop met agressieve
auto-mount kan dat theoretisch racen; op een **dedicated provisioning-station
kun je auto-mount het beste uitzetten** (bv. GNOME: `gsettings set
org.gnome.desktop.media-handling automount false`, of geen desktop draaien).

## Wat wel/niet wordt gepakt

- **Wel**: draaiende units, USB `2e8a:f00f` (de FatFS "PicoDisk"-schijf).
- **Niet**: blanco units in BOOTSEL (`2e8a:000f`, "RP2350", 128MB) — die moeten
  eerst geflasht worden (aparte productiestap: `firmware.uf2` naar de
  BOOTSEL-schijf kopiëren). Gewone USB-sticks worden ook genegeerd.

## Onderdelen

- `emisniffer_provisioner.py` — de app (Python + Tkinter, provisioning via mtools,
  geen mounten). Ook los te draaien: `sudo python3 emisniffer_provisioner.py`
  (vereist wel systeem-`mtools` en `python3-tk`).
- `build-appimage.sh` — bouwt de AppImage reproduceerbaar (bundelt een
  relocatable Python mét tkinter + mtools). Systeemafhankelijkheden op het
  doelsysteem: glibc, een X-display, en `eject` (util-linux) — die laatste is
  nodig om de FatFSUSB-write naar de flash te flushen (`fuser`/`pkill` worden
  best-effort gebruikt om mount-houders vrij te geven).

## Belangrijk: persistentie via eject

FatFSUSB commit host-writes pas naar de flash bij een echte **SCSI-eject**
(niet bij een kale unmount). De provisioner doet daarom na het schrijven een
`eject`. Ook wanneer je zelf `calibration.txt` bewerkt: **werp de schijf netjes
uit** (safely remove) — anders blijft de wijziging in de PC-cache en is 'ie na
een replug weer weg. Op Linux-desktop kan een bestandsbeheerder de eject
blokkeren ("busy"); zie ook `tools/set-cal.sh` voor een betrouwbare CLI-weg.

## Bouwen

```bash
./build-appimage.sh
```

De AppImage draait op x86_64 Linux met glibc >= die van de build-machine.
