# EMIsniffer firmware

Firmware for the EMIsniffer (RP2354): EMI VU meter, stereo balance, Li-Po
power/charge, digital volume pot, and a USB disk for `calibration.txt`.

The firmware itself is the same on every host OS. **`tools/*` are Linux-only.**
Windows users build with PlatformIO, then flash, provision, and calibrate by
hand (Explorer / Device Manager / a serial terminal).

---

## Common

### Prerequisites

- Python 3
- USB access to the board
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html), with up-to-date packages

Install PlatformIO into its own venv (do not `pip install platformio` into
system Python):

```bash
python3 get-platformio.py
```

On Windows, use `py get-platformio.py` if `python3` is not on `PATH`. The
installer puts Core in `%USERPROFILE%\.platformio\penv\`. Put that `Scripts`
(Windows) or `bin` (Linux) directory **first** on `PATH` so a stale
`pio` from pip does not win.

```text
Linux:   export PATH="$HOME/.platformio/penv/bin:$PATH"
Windows: set PATH=%USERPROFILE%\.platformio\penv\Scripts;%PATH%
```

Check with `pio --version`.

### Build

From the repo root:

```bash
pio run
```

| File | Use |
|---|---|
| `.pio/build/rp2354/firmware.uf2` | USB / BOOTSEL flash |
| `.pio/build/rp2354/firmware.hex` | SWD (J-Link / OpenOCD) |
| `.pio/build/rp2354/firmware.elf` | debug |

Do **not** use `pio run -t upload`. On a running unit PlatformIO can reset the
wrong serial port and copy the UF2 onto the FatFS **data** disk instead of the
BOOTSEL volume.

### SWD

`pio run` always refreshes `.pio/build/rp2354/firmware.hex` (XIP at
`0x10000000`). Flash the application only — a mass-erase wipes FatFS
(`calibration.txt` starts near `0x100FF000`).

### Serial protocol

115200 8N1. Only one program can open the port. Enable **DTR** and **RTS**;
TinyUSB CDC stays silent until those are asserted. Hardware flow control off.

After `ST: START` the firmware stops unsolicited debug and only answers commands
(`\n` or `\r\n`):

| Command | Reply |
|---|---|
| `ST: START` | `ST: OK` — quiet mode, LEDs as set by the command |
| `ST: QUIT` | resume normal VU / debug |
| `BATT` | millivolts, integer |
| `ADC=ALL` | `ADC: {ADC0}, {ADC1}, {ADC2}` |
| `ADC=0` / `ADC=1` / `ADC=2` | that channel, 12-bit 0..4095 |
| `LED=r,g,b` | `LED: OK` — all 13 LEDs plus one extra (DOUT of LED 13) |
| `PINTEST` / `SWEEP` / `GPIO29` / `ADCDBG` | bring-up loops until `ST: QUIT` |

### Buttons (on the unit)

| Action | Effect |
|---|---|
| Short press GPIO20 | volume + |
| Hold GPIO20 2 s | power off |
| Short press GPIO21 | volume − |
| Hold GPIO21 2 s | centre the balance LED (not saved; redo after power-on) |

Product toggles (filesystem, battery cutoff, …) live in `src/config.hpp`, not
in compiler `-D` flags.

---

## Linux

USB: your user typically needs group `dialout`. Scripts use sysfs, `lsusb`,
`mtools`, and SCSI `eject`.

Put PlatformIO on `PATH` permanently in `~/.zshrc` / `~/.bashrc`:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

### Flash over USB

```bash
./tools/flash-unit.sh
```

That builds, finds a blank unit already in BOOTSEL **or** a running unit
(`2e8a:f00f`), 1200 bps-resets it to BOOTSEL, and copies the UF2 onto the
volume labelled **RP2350**.

To force the bootloader by hand: hold BOOTSEL while plugging in USB, then copy
`firmware.uf2` onto that volume.

### First-time disk (provisioning)

Flashing does not write the volume label or `calibration.txt`. On a **running**
unit (USB disk `2e8a:f00f`):

```bash
./tools/provisioner/build/*.AppImage   # or see its README to run from source
```

See [tools/provisioner/README.md](tools/provisioner/README.md). Blank boards in
BOOTSEL are ignored until they are flashed.

### Serial

CDC is typically `/dev/ttyACM0` or `/dev/ttyACM1` (another ACM port may be a
J-Link).

```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

### Calibration

The unit mounts as USB disk **EMISNIFFER** with `calibration.txt`. Edit, save,
and **eject** (SCSI eject, not a bare unmount) or the change never reaches flash.

```bash
./tools/set-cal.sh 200 2000
./tools/set-cal.sh 200 2000 85 1.17
```

---

## Windows

There are no `tools/*.sh` / provisioner helpers. After `pio run`, do flash,
provision, and calibration in a command window or from an IDE, such as Visual Studio Code.

Add PlatformIO to `PATH` for the session (or via Environment Variables):

```bat
set PATH=%USERPROFILE%\.platformio\penv\Scripts;%PATH%
```

### Flash over USB

1. Build with `pio run`.
2. Put the unit in BOOTSEL: hold BOOTSEL while plugging in USB, **or** (if it
   is already running) use a serial terminal to open the Pico CDC at **1200**
   baud and toggle DTR — that resets to BOOTSEL. Pick the COM port whose USB
   IDs are `2e8a:f00f` (running firmware), not a J-Link.
3. A drive labelled **RP2350** appears. Copy
   `.pio\build\rp2354\firmware.uf2` onto it. The unit reboots by itself.

Do not copy the UF2 onto **EMISNIFFER** / PicoDisk — that is the data
filesystem, not the bootloader.

### First-time disk (provisioning)

After firmware is running, Windows shows a small USB volume (often still
labelled PicoDisk until you rename it).

1. Set the volume label to **EMISNIFFER** (Properties on the drive).
2. Create `calibration.txt` in the root with at least:

   ```
   adc_min=730
   adc_max=4095
   smoothing=85
   vbat_cal=1.17
   ```

3. **Eject** the drive (Safely Remove / Eject). A plain unmount is not enough;
   FatFSUSB only writes to flash on a SCSI eject. Then replug if you need the
   disk again.

### Serial

Device Manager → Ports: Pico CDC is `COMn`. Another COM port may be a J-Link.

```bat
pio device monitor -p COMn -b 115200
```

In PuTTY / CuteCom: 115200, DTR on, RTS on, no hardware flow control.

### Calibration later

Open the **EMISNIFFER** drive, edit `calibration.txt`, save, **eject**. Same
eject rule as provisioning.
