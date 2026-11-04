#!/bin/bash
# p4browser flash script — run ON THE MAC (or via ssh into the Mac).
# Finds the ESP32-P4 USB serial port and flashes the app + bootloader + partitions.
# Usage: ./flash.sh [port]
set -e
PORT="${1:-}"
APP="/Users/Shared/p4browser/build"   # build dir copied from NAS; override via $APP
B="build"
if [ -n "$PORT" ]; then
  :
else
  # Auto-detect: ESP32-P4 USB shows as a cu.usb* / tty.usb* device
  PORT=$(ls /dev/cu.usb* /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -z "$PORT" ]; then
    echo "No USB serial device found. Available:"
    ls /dev/cu.* 2>/dev/null
    exit 1
  fi
fi
echo "Flashing to $PORT"
# esptool with full image
python3 -m esptool --chip esp32p4 -p "$PORT" -b 921600 \
  --before default_reset --after hard_reset \
  write_flash \
  0x0 "$B/bootloader/bootloader.bin" \
  0x8000 "$B/partition_table/partition-table.bin" \
  0xf000 "$B/phy_init.bin" \
  0x90000 "$B/p4browser.bin"
echo "FLASH_DONE"
