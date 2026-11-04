# Architecture

p4browser has **two rendering modes**, selected at runtime by whether the
remote renderer is connected:

1. **Remote render mode (default in production).** A headless Chromium on a
   network machine renders the web page; the board is a thin display client:
   JPEG in over WebSocket, decoded in hardware, touched back out. This is how
   the Homey Dashboard Studio dashboard is shown — the page's own JS runs the
   widgets, and touch events drive them.
2. **Local mode.** The board's own LVGL 9 UI: a text-mode web browser
   (phone-as-keyboard: `GET /url?u=...` on the board's HTTP server), the
   boot screen, and the config screen. Used for setup and as a fallback.

Both modes share one physical display path (DSI panel + LVGL), and the local
UI is drawn *on top of* the video path's frame buffer (see
`firmware.md` §Framebuffers).

## Hardware

| Block | Detail |
|---|---|
| SoC | ESP32-P4, dual-core RISC-V @ 400 MHz, 32 MB PSRAM, 16 MB flash |
| Wi-Fi | ESP32-C6 co-processor over SDIO (`esp_wifi_remote` + `esp_hosted`); C6 firmware embedded in the app (`main/c6_fw_2_12_12.bin`) |
| Panel | 10.1" MIPI-DSI, **800×1280 RGB565**, 2-lane, driven via the DPI driver + DMA2D; **physically portrait**, mounted landscape |
| Touch | GSL3680 capacitive, I2C, INT + RST GPIOs |
| USB | CH340 serial (flashing/diagnostic console) |
| LED | Backlight via LEDC PWM on GPIO23 (`/brightness`) |

## Frame pipeline (remote mode)

```
Chrome compositor (1280×800)
  │  page.screenshot() → PNG
  ▼
renderer.js (NAS)
  │  decode → downsample fingerprint (change detection)
  │  JPEG encode (q = RENDERER_QUALITY, default 70)
  │  WebSocket JSON+binary → ws://board:9100
  ▼
remote.c (board)
  │  HW JPEG decoder (esp_driver_jpeg) → 1280×800 RGB888 PSRAM buffer
  │  software convert RGB888→RGB565 + 90° rotation
  │  → 800×1280 RGB565 frame buffer (the one DSI is scanning out)
  ▼
MIPI-DSI (DMA2D) → panel
```

Measured costs (this board): screenshot ≈ **160 ms** on the NAS, JPEG
decode + convert + rotate ≈ **150–250 ms** on the board. The renderer sends at
up to 8 fps on a changing page; a static page sends only heartbeat frames
(default 5 s) — traffic is near zero until something changes.

**Why JPEG over WS and not raw frames?** 1280×800 RGB888 is 3 MB per frame —
an 8 fps stream would be ~24 MB/s sustained, which degrades the board's lwIP
stack within ~15–20 minutes. JPEG q70 is ~100–300 KB per frame, and the P4's
hardware JPEG decoder makes the cost trivial.

## Touch pipeline (remote mode)

```
GSL3680 (finger)
  │  touch task, 5 ms poll, I2C
  ▼
remote.c
  │  touch_to_page(): panel coords (800×1280 portrait fb) → page coords (1280×800)
  │  accounting for the DSI mirror_x and the 90° rotation
  │  down/move/up JSON → WebSocket
  ▼
renderer.js
  │  CDP Input.dispatchMouseEvent over Chrome DevTools port
  ▼
page JavaScript (widget handlers)
```

The board's LVGL input device is **bypassed in remote mode** — touch belongs
to the page. In local mode the same GSL3680 data feeds LVGL's indev.

## Rotation and the landscape standard

The panel is physically portrait (800 wide × 1280 tall); the standard layout
is **landscape 1280×800**. Two independent rotations exist and both must
agree:

- **Video path:** `remote.c` rotates each decoded 1280×800 RGB888 frame 90°
  into the 800×1280 fb (logical point `(px,py)` → fb `(py, 1279−px)`).
- **LVGL path:** `lv_display_set_rotation(LV_DISPLAY_ROTATION_90)` makes the
  logical LVGL screen 1280×800; in PARTIAL render mode the flush callback
  performs a manual `lv_draw_sw_rotate` of each dirty area into a dedicated
  PSRAM scratch buffer before writing the fb, so LVGL's logical coordinates
  land in the same physical places as the video path. Touch coordinates are
  converted 1:1 by LVGL's rotation.

If the two disagree, the local UI (config screen) appears rotated or mirrored
relative to the dashboard — the classic symptom is a mangled settings screen
while the dashboard looks fine.

## Framebuffers and local-UI overlay

- The DSI controller scans out the active frame buffer continuously.
- Video frames are written directly into the active fb (bypassing LVGL).
- LVGL runs in **PARTIAL** render mode with its own PSRAM draw buffers; its
  flush callback writes only dirty regions into the *same* fb. The local UI
  (title/status bar, config screen, boot screen) therefore overlays the video
  with no extra full-frame traffic.
- `/state` shows `remap`/`order` — the buffer-remapping used so the video
  path never fights the scanout.

## Control & diagnostics

The board runs an HTTP server on port 80:

| Endpoint | Purpose |
|---|---|
| `GET /state` | Full status snapshot (the primary diagnostic; see `firmware.md` §/state) |
| `GET /health` | Liveness |
| `POST /config` | Wi-Fi / renderer endpoint / brightness (persists to NVS) |
| `GET /url?u=` | Local-mode navigation (phone-as-keyboard) |
| `GET /brightness?level=` | Backlight 0–100 (LEDC PWM, persisted) |
| `GET /screen?cfg=0\|1` | Open/close config screen |
| `GET /reboot` | Reboot |
| `GET /factory` | Factory reset (NVS wipe) |
| `GET /fill` | Solid-color panel fill test (direct CPU write, cache-synced) |

## Deployment topology (reference)

- **Board:** `192.168.3.129` on the home LAN (Wi-Fi, DHCP).
- **NAS renderer:** systemd *user* service `nas-renderer` (Synology DSM —
  user services need `XDG_RUNTIME_DIR=/run/user/<uid>`; see
  `build-and-flash.md` §DSM notes). Chrome headless shell + node 22.
- **Homey Pro:** `192.168.3.5`; Dashboard Studio serves `viewer.html` on
  `:5050`. The renderer's window is sized to the dashboard canvas
  (1280×800).
