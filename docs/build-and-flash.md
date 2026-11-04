# Build & flash

## Firmware

### Toolchain

ESP-IDF **5.5** (`idf.py set-target esp32p4`). The `tools/idf55` wrapper shows
the manual-env approach used on the Synology NAS (its `export.sh`
`detect_python.sh` clobbers `"$@"`, so the env is exported by hand — see the
script for the exact exports and two IDF 5.5 compatibility notes:
`IDF_MAINTAINER=1` to demote the toolchain-version gate, and the
`esp_hosted` Kconfig `$(ESP_IDF_VERSION)` macro neutralized for 5.5).

On a normal dev machine:

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32p4
idf.py build
```

Component manager downloads LVGL 9.3.0, `esp_wifi_remote`, `esp_hosted`
2.12.12 and the GSL3680 driver into `managed_components/` (gitignored;
pinned in `dependencies.lock`).

### Flash

The P4's USB serial is usually on a machine with a free USB port (in this
deployment: a Mac, over the CH340). The build artifacts live on the build
machine; copy them to the flashing machine (e.g. `tools/macssh.py put`), then:

```sh
esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 bootloader.bin \
  0x8000 partition-table.bin \
  0xd000 ota_data_initial.bin \
  0x10000 p4browser.bin
```

(`tools/flash.sh` automates port detection on the Mac; `tools/macssh.py`
wraps `ssh`/`sftp` to the Mac with credentials from the environment —
`MAC_SSH_HOST` / `MAC_SSH_USER` / `MAC_SSH_PASSWORD`.)

Expected: ~3.0 MB written, hash verified, then `hard_reset`. Boot takes ~15 s
(Wi-Fi + renderer connect). Verify:

```sh
curl http://<board-ip>/state
```

Board IP in the reference deployment: `192.168.3.129` (DHCP on the home
LAN). The serial console (`cu.usbserial*` on the Mac) is **ground truth** for
boot problems — see troubleshooting.

## Renderer

### Setup

```sh
cd renderer
npm install                 # puppeteer-core, ws
python3 fetch_chrome.py     # downloads chrome-headless-shell (or point puppeteer at an existing Chrome)
```

Fonts: the reference deployment ships a font cache/config directory next to
the renderer (`fontcache/`, `fontconf/`) so the dashboard's webfonts render
deterministically headlessly.

### Run (development)

```sh
PORT=9100 PIN_URL=http://<homey>:5050/viewer.html RENDERER_LOG=/tmp/renderer.log node renderer.js
```

### Run as a systemd user service (Synology DSM)

Template in `systemd/nas-renderer.service`. Install:

```sh
mkdir -p ~/.config/systemd/user
cp renderer/systemd/nas-renderer.service ~/.config/systemd/user/
# edit: ExecStart path, PIN_URL, RENDERER_LOG
systemctl --user daemon-reload
systemctl --user enable --now nas-renderer
```

**DSM notes (hard-won):**

- DSM's user systemd needs `XDG_RUNTIME_DIR=/run/user/<uid>` in the calling
  shell: `XDG_RUNTIME_DIR=/run/user/1026 systemctl --user ...`.
- `journalctl --user` returns "No journal files were found" for user services
  on this DSM — **use `RENDERER_LOG`** (the renderer logs everything there,
  including the per-5 s `cap=` stats and watchdog events).
- The unit runs the watchdog (`renderer_watchdog.py`), which respawns node;
  `Restart=always` covers the watchdog itself.

## Configuration (board UI)

1. Long-press the screen for 3 s → config screen.
2. Set Wi-Fi (if not already on the right network), renderer host/port, and
   the dashboard URL.
3. **Save&apply** — Wi-Fi reconnects / renderer endpoint hot-restarts.
4. Brightness: slider on the config screen, or `GET /brightness?level=0-100`
   (persisted, LEDC PWM on GPIO23).

## Operational endpoints (board, port 80)

`/state` · `/health` · `/config` (POST) · `/url?u=` (local-mode nav) ·
`/brightness?level=` · `/screen?cfg=0|1` · `/reboot` · `/factory` · `/fill`
(panel fill test) — see `architecture.md` §Control & diagnostics.
