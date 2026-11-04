#!/usr/bin/env python3
"""Force a real boot into the flashed app on the p4browser.

esptool leaves the chip in ROM download mode. The ROM bootloader accepts
ASCII bytes: 'r' (0x72) = run application from flash. This script:
  1. sends a default_reset via esptool to enter download mode (no reset after)
  2. opens the UART and sends b'r' to make the ROM boot flash (build 90)
"""
import sys, time, serial, subprocess

PORT = "/dev/cu.usbserial-10"
BAUD = 460800
ESP = "/Users/peter_martis/esptool-venv/bin/python"

# 1. Enter download mode, do NOT reset out (leaves chip waiting in ROM)
p = subprocess.run([ESP, "-m", "esptool", "--chip", "esp32p4", "-b", str(BAUD),
                    "-p", PORT, "--before", "default_reset", "--after", "no_reset",
                    "chip_id"], capture_output=True, text=True)
out = p.stdout + p.stderr
print("esptool:", [l for l in out.splitlines() if "chip id" in l.lower() or "error" in l.lower() or "Failed" in l or "download" in l.lower()][:4])

# 2. Open UART, send the ROM run byte a few times
time.sleep(0.3)
try:
    s = serial.Serial(PORT, BAUD, timeout=1, write_timeout=2, rtscts=False, dsrdtr=False)
    time.sleep(0.2)
    for _ in range(5):
        s.write(b'r')
        s.flush()
        time.sleep(0.15)
    s.close()
    print("SENT r-bytes — board should be booting build 90")
except Exception as e:
    print("SERIAL_FAIL:", e)
