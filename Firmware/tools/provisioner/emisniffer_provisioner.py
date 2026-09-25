#!/usr/bin/env python3
"""EMIsniffer provisioning station.

Bewaakt USB: zodra een DRAAIENDE EMIsniffer-unit (USB 2e8a:f00f, de FatFS
'PicoDisk'-schijf) wordt ingeplugd, zet dit programma automatisch het
volumelabel op EMISNIFFER en schrijft een standaard calibration.txt.

- Blanco units in BOOTSEL (2e8a:000f, 'RP2350', 128MB) worden GENEGEERD:
  die moeten eerst geflasht worden (aparte productiestap).
- Provisioning gebeurt met mtools (mlabel/mcopy) rechtstreeks op het rauwe
  block-device, dus zonder mounten. Vereist root (rauwe device-schrijftoegang).

Los te draaien:   sudo python3 emisniffer_provisioner.py
Als AppImage:     sudo ./EMIsniffer-Provisioner-x86_64.AppImage
"""

import os
import sys
import glob
import time
import shutil
import tempfile
import subprocess
import tkinter as tk
from tkinter import scrolledtext

# --- Doel-unit ---------------------------------------------------------------
USB_VID = "2e8a"
USB_PID = "f00f"          # draaiende firmware (BOOTSEL = 000f, wordt genegeerd)
VOL_LABEL = "EMISNIFFER"  # FAT-label: max 11 tekens, hoofdletters

CALIBRATION_TXT = (
    "# VU-meter kalibratie\r\n"
    "# Bewerk waarden, sla op en WERP DE SCHIJF UIT (het board herlaadt dan).\r\n"
    "# adc_min = ADC0 -> 0 segmenten, adc_max = ADC0 -> 8 segmenten, smoothing = 0..95\r\n"
    "\r\n"
    "adc_min=730\r\n"
    "adc_max=4095\r\n"
    "smoothing=85\r\n"
)

POLL_MS = 1000


# --- mtools-lokalisatie (gebundeld in AppImage, anders systeem) --------------
def _tool(name):
    appdir = os.environ.get("APPDIR")
    if appdir:
        cand = os.path.join(appdir, "usr", "bin", name)
        if os.path.exists(cand):
            return cand
    found = shutil.which(name)
    return found or name


# --- USB-detectie via sysfs --------------------------------------------------
def _usb_ids(devname):
    """Loop vanaf /sys/block/<devname> omhoog tot een USB-device met idVendor."""
    start = os.path.realpath(os.path.join("/sys/block", devname, "device"))
    d = start
    for _ in range(12):
        vid = os.path.join(d, "idVendor")
        pid = os.path.join(d, "idProduct")
        if os.path.exists(vid) and os.path.exists(pid):
            try:
                with open(vid) as f:
                    v = f.read().strip().lower()
                with open(pid) as f:
                    p = f.read().strip().lower()
                return v, p
            except OSError:
                return None, None
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None, None


def find_target_units():
    """Geef {devnode: serial} voor alle ingeplugde 2e8a:f00f block-devices."""
    units = {}
    for path in glob.glob("/sys/block/sd*"):
        name = os.path.basename(path)
        vid, pid = _usb_ids(name)
        if vid == USB_VID and pid == USB_PID:
            units["/dev/" + name] = _serial(name)
    return units


def _serial(devname):
    d = os.path.realpath(os.path.join("/sys/block", devname, "device"))
    for _ in range(12):
        s = os.path.join(d, "serial")
        if os.path.exists(s):
            try:
                with open(s) as f:
                    return f.read().strip()
            except OSError:
                return devname
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return devname


# --- Provisioning ------------------------------------------------------------
def _mountpoints(dev):
    """Alle mountpoints van dev (bv. door desktop-auto-mount)."""
    mps = []
    try:
        with open("/proc/mounts") as f:
            for line in f:
                src, mnt = line.split()[:2]
                if src == dev:
                    mps.append(mnt.encode().decode("unicode_escape"))
    except OSError:
        pass
    return mps


def provision(dev):
    """Zet label + calibration.txt op het rauwe device via mtools.
    Retourneert (ok: bool, log: str)."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    lines = []

    def run(cmd):
        lines.append("$ " + " ".join(cmd))
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=30)
        out = (r.stdout + r.stderr).strip()
        if out:
            lines.append(out)
        return r.returncode == 0

    mlabel, mcopy, minfo = _tool("mlabel"), _tool("mcopy"), _tool("minfo")
    eject = shutil.which("eject") or "eject"

    # Houders vrijgeven + unmounten zodat mtools rauw kan schrijven (een blijvende
    # holder blokkeert straks de eject-flush -> wijziging zou niet persisteren).
    subprocess.run(["pkill", "-x", "nautilus"], capture_output=True)
    subprocess.run(["fuser", "-km", dev], capture_output=True)
    time.sleep(0.5)
    if _mountpoints(dev):
        lines.append("# unmount " + dev)
        if subprocess.run(["umount", dev], capture_output=True).returncode != 0:
            subprocess.run(["udisksctl", "unmount", "-b", dev], capture_output=True)
        time.sleep(0.3)

    tmp = None
    try:
        # 1) label zetten
        if not run([mlabel, "-i", dev, "::" + VOL_LABEL]):
            return False, "\n".join(lines) + "\n-> label zetten mislukt"

        # 2) calibration.txt schrijven
        fd, tmp = tempfile.mkstemp(prefix="calib_", suffix=".txt")
        with os.fdopen(fd, "wb") as f:
            f.write(CALIBRATION_TXT.encode("ascii"))
        if not run([mcopy, "-i", dev, "-o", tmp, "::calibration.txt"]):
            return False, "\n".join(lines) + "\n-> calibration.txt schrijven mislukt"

        # 3) verificatie (vóór de eject; daarna is het device weg)
        run([minfo, "-i", dev])

        # 4) ECHTE SCSI-eject -> firmware flusht naar flash. Zonder dit blijft de
        #    wijziging in RAM en is 'ie na een replug weer weg!
        if not run([eject, dev]):
            return False, "\n".join(lines) + "\n-> eject/flush mislukt (niet in flash!)"

        return True, "\n".join(lines)
    except subprocess.TimeoutExpired:
        return False, "\n".join(lines) + "\n-> time-out (device losgeraakt?)"
    except Exception as e:  # noqa: BLE001
        return False, "\n".join(lines) + f"\n-> fout: {e}"
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)


# --- GUI ---------------------------------------------------------------------
class App:
    OK, FAIL, WAIT = "#1a7f37", "#b3261e", "#5f6368"

    def __init__(self, root):
        self.root = root
        self.provisioned = {}   # devnode -> serial (nu ingeplugd + al gedaan)
        self.count = 0
        root.title("EMIsniffer Provisioner")
        root.geometry("640x480")

        self.status = tk.Label(root, text="WACHTEN OP UNIT…", font=("Sans", 28, "bold"),
                               fg="white", bg=self.WAIT, height=2)
        self.status.pack(fill="x")

        info = tk.Frame(root)
        info.pack(fill="x", padx=10, pady=6)
        self.detail = tk.Label(info, text="Plug een geflashte unit in.", font=("Sans", 12), anchor="w")
        self.detail.pack(side="left")
        self.counter = tk.Label(info, text="OK: 0", font=("Sans", 12, "bold"), anchor="e")
        self.counter.pack(side="right")

        self.log = scrolledtext.ScrolledText(root, font=("Monospace", 9), height=18)
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        if os.geteuid() != 0:
            self._log("WAARSCHUWING: niet als root gestart — label zetten kan falen. "
                      "Start met sudo.\n")

        self.root.after(300, self.poll)

    def _log(self, msg):
        self.log.insert("end", msg + ("\n" if not msg.endswith("\n") else ""))
        self.log.see("end")

    def set_status(self, text, color):
        self.status.config(text=text, bg=color)

    def poll(self):
        try:
            current = find_target_units()
        except Exception as e:  # noqa: BLE001
            current = {}
            self._log(f"detectiefout: {e}")

        # verdwenen units vergeten (zodat herinpluggen opnieuw provisiont)
        for dev in list(self.provisioned):
            if dev not in current:
                self.provisioned.pop(dev, None)

        # nieuwe units provisionen
        for dev, serial in current.items():
            if dev in self.provisioned:
                continue
            self.provisioned[dev] = serial
            self._provision_ui(dev, serial)

        if not current:
            self.set_status("WACHTEN OP UNIT…", self.WAIT)
            self.detail.config(text="Plug een geflashte unit in.")

        self.root.after(POLL_MS, self.poll)

    def _provision_ui(self, dev, serial):
        self.set_status("BEZIG…", self.WAIT)
        self.detail.config(text=f"{dev}  (serial {serial})")
        self.root.update_idletasks()
        ok, log = provision(dev)
        self._log(f"=== {dev}  serial={serial} ===")
        self._log(log)
        if ok:
            self.count += 1
            self.counter.config(text=f"OK: {self.count}")
            self.set_status("KLAAR ✓  UNIT LOSKOPPELEN", self.OK)
            self._log("-> OK\n")
        else:
            self.set_status("FOUT ✗  ZIE LOG", self.FAIL)
            self._log("-> MISLUKT\n")


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
