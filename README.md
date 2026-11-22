# p4browser

A full web-rendering browser for the **Guition JC8012P4A1** (ESP32-P4, 10.1"
800×1280 MIPI-DSI touchscreen). A headless Chromium running on any always-on
Linux box — a Synology NAS in this deployment, but a **Raspberry Pi** or any
Linux server works just as well — renders any web page (in this project, the
**Homey Pro Dashboard Studio** home-automation dashboard) and streams the
frames over the LAN as JPEGs to the ESP32-P4, which decodes them in hardware
and drives the DSI panel. Touch input on the board travels back over the same
WebSocket and is injected into the page via Chrome DevTools Protocol, so the
dashboard's own controls (switches, sliders, widgets) work directly on the
wall display.

```
   ┌──────────── Renderer: NAS / Pi ─────────────┐        ┌────────────────────── Board ──────────────────────┐
   │  headless Chrome 1280×800                    │  WS    │  remote.c                                          │
   │  renderer.js: capture → diff → JPEG q70  ────┼───────┼─▶ HW JPEG decode → RGB888 1280×800                │
   │         ▲ touch injected via CDP (CDP :9222) │  :9100 │         │  RGB888→RGB565 + 90° rotate (~150ms)    │
   │  control HTTP :5052 (/reload /nav /status)   │ ◀──────┼─── touch JSON (GSL3680, 5ms poll)                │
   │  puppeteer-core, ws, node 22                 │        │  DSI double-buffered fb (800×1280 RGB565)        │
   │  systemd user service (watchdog)             │        │  LVGL 9 (local UI: config screen, gestures)      │
   └──────────────────────────────────────────────┘        │  HTTP server :80 (/state /config /url /brightness)│
                                                            └──────────────────────────────────────────────────┘
```

## Repository layout

| Path | What it is |
|---|---|
| `firmware/` | ESP-IDF 5.5 project for the JC8012P4A1 (LVGL 9.3, esp_hosted C6 Wi-Fi, HW JPEG) |
| `firmware/main/` | `browser.c` (display + local text browser + config screen wiring), `remote.c` (remote render client + touch), `settings.c` (config screen), `main.c` (HTTP server, app_main, DSI init), `p4_jc8012.c` (board init sequence) |
| `firmware/tools/` | Build wrappers (`idf55`), flash scripts, `macssh.py` (remote flashing helper) |
| `firmware/prebuilt/` | Latest prebuilt `p4browser.bin` (+ `SHA256`, `VERSION`) for flashing without an ESP-IDF toolchain |
| `renderer/` | Node.js renderer: `renderer.js` (capture/diff/send loop + CDP touch injection + plain-HTTP control server on :5052 for `/reload` `/nav` `/status`), `dashboard-server.js` (optional Homey API bridge for a simple toggle dashboard), `renderer_watchdog.py` (respawning supervisor), `systemd/` service templates for a NAS and a Raspberry Pi |
| `docs/` | Architecture, firmware, renderer, build/flash, troubleshooting, and the original design documents |

## Quick start

> **No ESP-IDF toolchain?** Flash the prebuilt image in
> [`firmware/prebuilt/p4browser.bin`](firmware/prebuilt/p4browser.bin) with the
> `esptool` command below (check `firmware/prebuilt/SHA256` first). Once a board
> is up, later firmware updates ship over the LAN: `curl -X POST
> --data-binary @p4browser.bin http://<board>/ota`.

**Firmware** (build on any machine with ESP-IDF 5.5; this project was built
on a Synology NAS):

```sh
cd firmware
idf.py set-target esp32p4
idf.py build          # → build/p4browser.bin
```

Flash (USB serial port, e.g. the CH340 on a Mac):

```sh
esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin \
  0x10000 build/p4browser.bin
```

**Renderer** (on the LAN machine, Node 22+ — a Synology NAS, a Raspberry Pi,
or any always-on Linux box):

```sh
cd renderer
npm install
python3 fetch_chrome.py      # downloads chrome-headless-shell (skip on a Pi —
                             # use the system Chromium; see docs/build-and-flash.md)
node renderer.js             # or run as a systemd user service (NAS or Pi)
```

Point the renderer at your dashboard URL (`RENDERER_URL` / the `connect`
message from the board) and open the board's config screen
(3-second long-press) to set the renderer host/port and Wi-Fi.

Full instructions: [`docs/build-and-flash.md`](docs/build-and-flash.md).

## Reliability

The board and renderer are built to survive a renderer host (or the whole NAS)
rebooting underneath them:

- **Boot config fallback.** If no frame reaches the board within ~30 s of boot,
  it shows the config screen with the right instruction (no Wi-Fi → set SSID;
  Wi-Fi but no stream → check the renderer host/port) instead of a black panel,
  and resumes video the instant frames return. See
  [`docs/troubleshooting.md`](docs/troubleshooting.md) §Board black/garbled after
  a NAS reboot.
- **Renderer autostart after a host reboot.** systemd user units + linger are
  *necessary but not sufficient* on Synology DSM — the reliable mechanism is a
  root `rc.d` boot trigger that starts the user services explicitly. Make the
  volatile DSM journal persistent so a future boot is diagnosable.
- **Socket watchdogs + self-heal** keep a half-dead WebSocket or a hung page
  from wedging the display (see [docs/troubleshooting.md](docs/troubleshooting.md)).

## Documentation

- [Architecture](docs/architecture.md) — the two rendering modes, the frame pipeline, the touch pipeline, latency budget
- [Firmware](docs/firmware.md) — file-by-file tour, display/rotation, config screen & gestures, `/state` reference
- [Renderer](docs/renderer.md) — capture loop, change-detection, animation guard & input bypass, self-heal, the plain-HTTP control server (`/reload` `/nav` `/status`), env vars
- [Build & flash](docs/build-and-flash.md) — firmware build/flash, renderer deployment on a NAS **or a Raspberry Pi**, systemd services
- [Troubleshooting](docs/troubleshooting.md) — the hard-won lessons: DSI init, lwIP animation floods, tap-latency diagnosis, crash forensics
- [Design history](docs/assignment.md) and [Remote-render plan](docs/renderer-plan.md) — the original handoff documents

## Hardware notes

- Board: Guition JC8012P4A1 — ESP32-P4 (dual-core RISC-V @ 400 MHz, 32 MB
  PSRAM, 16 MB flash), ESP32-C6 co-processor over SDIO (Wi-Fi), 10.1"
  MIPI-DSI 800×1280 panel, GSL3680 capacitive touch.
- The physical panel is **portrait 800×1280**; the standard layout is
  **landscape 1280×800** — the video pipeline and LVGL both rotate 90°
  (see `docs/firmware.md` §Rotation).
- The renderer host needs Node 22+ and a headless Chromium. Any always-on
  Linux box works — see [`docs/build-and-flash.md`](docs/build-and-flash.md)
  §Raspberry Pi for a no-NAS deployment.

## License

MIT — see [LICENSE](LICENSE).
