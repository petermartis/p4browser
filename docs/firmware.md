# Firmware

ESP-IDF 5.5 project for the Guition JC8012P4A1. Target `esp32p4`. Component
manager pulls LVGL 9.3.0, `esp_wifi_remote`, `esp_hosted` 2.12.12 and the
GSL3680 touch driver — see `main/idf_component.yml` and `dependencies.lock`.

## Files

| File | Role |
|---|---|
| `main/main.c` | `app_main`: PSRAM, NVS, C6 Wi-Fi bring-up, DSI panel init, LVGL display init, backlight (LEDC PWM GPIO23), HTTP server with all endpoints |
| `main/p4_jc8012.c` + `jc8012_base_seq.*` | Board bring-up: regulator/panel init sequence (the proven DSI path — **do not modify** once the panel works) |
| `main/browser.c` | LVGL display port (flush callback with rotation), local text-mode browser, boot screen, title/status bar, local-UI-active gating, 3 s long-press gesture |
| `main/remote.c` | Remote render client: WebSocket client, HW JPEG decode, RGB888→RGB565 + rotate, frame presentation, touch task + `touch_to_page()`, NVS renderer endpoint, reconnect watchdog |
| `main/settings.c` / `settings.h` | Config screen (landscape 1280×800 layout): Wi-Fi, renderer host/port/URL, brightness slider, 4 action buttons, LVGL keyboard |
| `main/c6_fw_2_12_12.bin` | ESP32-C6 Wi-Fi firmware, embedded via `EMBED_FILES` |

## Display init

The DSI panel is driven by the `esp_lcd` DPI driver: 2-lane, 800×1280 RGB565,
DMA2D color conversion. Init order in `main.c`:

1. PSRAM (octal) — all large buffers (fb, JPEG output, LVGL draw buffers,
   rotation scratch) live in PSRAM.
2. `p4_jc8012` board sequence (regulators, reset, panel init from the base
   sequence table).
3. DPI panel device + LCD panel + touch (GSL3680) drivers.
4. Framebuffer in PSRAM, panel start.
5. LVGL display attach (RGB565, 800×1280 physical) + **rotation** (below).

The panel init sequence (including its `mirror_x` term) was verified against
a working reference firmware — the display path in `main.c` is *proven*;
changes to it are the top cause of "black screen / garbage" regressions.

## Rotation (landscape standard)

The panel is physically portrait; the standard layout is landscape 1280×800.

```c
lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);
```

This makes the *logical* LVGL screen 1280×800. LVGL 9.3's built-in rotation
assumes FULL render mode; in PARTIAL mode the flush callback must rotate each
dirty area itself:

```c
// flush callback (browser.c):
//   1. render into LVGL's PSRAM draw buffer (logical coords)
//   2. lv_draw_sw_rotate() the area 90° into s_rot_buf (dedicated PSRAM scratch)
//   3. esp_lcd_panel_draw_bitmap() the rotated area into the fb
```

The mapping must match the video path's (`remote.c`) rotation exactly:
logical `(px, py)` → fb `(fb_x = py, fb_y = 1279 − px)`. Corner check:
logical TL (0,0) → fb (0,1279); logical TR (1279,0) → fb (0,0). If a future
change makes the config screen look rotated/mirrored while the dashboard is
fine, this mapping is the place to look first.

## Color pipeline (remote frames)

`remote.c` receives JPEG, decodes **in hardware** (esp_driver_jpeg) into a
1280×800 **BGR888→RGB888** PSRAM buffer, then a software pass converts
RGB888→RGB565 while rotating 90° into the 800×1280 fb. Measured ~150 ms
per frame at this resolution (reported as `rot_ms` in `/state`).

Two hard-won rules:

- **Do not feed the HW JPEG decoder RGB565 or 420 chroma targets directly to
  DSI** — the pipeline above (RGB888 intermediate, software 565 convert) is
  the verified working order.
- After any *direct CPU write* to the fb (e.g. `/fill`), the data cache must
  be flushed (`esp_cache_msync`) before DMA2D reads it, or the panel shows
  stale/garbage.

## Local mode (text browser)

`browser.c` implements a minimal text browser for local use: `GET /url?u=...`
on the board queues a navigation; `browser_task` (core 1) fetches with
`esp_http_client` (built-in root CA), converts HTML → plain text, and
publishes to the LVGL task under the LVGL mutex (LVGL is not thread-safe).
The phone is the keyboard. This mode also provides the boot screen and the
title/status overlay bar in remote mode.

## Config screen & gesture

- **Gesture:** 3-second long-press opens the config screen. The detection
  runs on *raw* touch data (not LVGL indev), so it fires even while the
  remote page has the touch indev — the raw-fallback path was added because
  LVGL indev events are suppressed in remote mode.
- **Layout (landscape 1280×800):** title at top; a 1240×504 content box in
  three columns — Wi-Fi (SSID/password), Renderer (host/port/dashboard URL),
  Display (brightness slider); a wide LVGL keyboard; and four bottom buttons
  (Back / Save&apply / Reboot / Factory), each 290×80, evenly spaced
  (x = 30/345/660/975), 60 px above the bottom edge.
- **Persistence:** all settings persist to NVS; Save&apply reboots the
  relevant subsystems (Wi-Fi reconnect / renderer endpoint hot-restart).
- `/screen?cfg=1` opens the screen without the gesture (diagnostics).

## `/state` reference

`GET /state` is the primary diagnostic channel. Fields (grouped):

- `task=` / `nav=` — local browser task state and nav queue depth.
- `heap=` / `page=` — free heap and page-buffer usage.
- `remote:` — renderer client: connected state, `frames_rx`, `decoded`,
  `failed`, `frame_age` (ms since last frame — the staleness indicator),
  `rot_ms` (last frame decode+convert+rotate time), `pong_age` (WS health),
  `fb`/`remap`/`order` (framebuffer/presentation state).
- `touchhold:` — long-press detector: `n` (samples), `last`/`max` (ms),
  `gfires` (gesture fires), `fhist16s` (16-bit ring, 1 bit per 50 ms — a
  visual "fingerness" trace).
- `input_sent`, `touch_pts` — remote touch events sent to the renderer.

A healthy streaming board looks like:

```
remote: active frames_rx=151 decoded=150 failed=0 frame_age=527ms rot_ms=150
```

`frame_age` growing unbounded = renderer not sending (check the renderer log,
then `crash1` in `troubleshooting.md`). `failed` climbing = JPEG decode errors
(check quality/bitrate).

## Build & flash

See [build-and-flash.md](build-and-flash.md).
