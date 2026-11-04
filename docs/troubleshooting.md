# Troubleshooting

Hard-won lessons from bringing this system up. Order of trust for "what does
the screen actually show": **serial console > board `/state` > RTSP camera
still > vision model** (the last one hallucinated screen content repeatedly;
use OpenCV stills + a human instead).

## Diagnostic channels

| Channel | What it tells you | Gotcha |
|---|---|---|
| Board serial console (CH340 on the Mac) | Boot logs, task state, frame counters — **ground truth** | Need a serial reader (`pyserial` in a venv); the console is chatty, capture with a timeout |
| `GET /state` (board:80) | Remote client, frame age, touch gesture detector | `frame_age` growing = no frames arriving; see `firmware.md` §/state |
| `RENDERER_LOG` (NAS) | `cap=` per-frame stats, `[TOUCH-IN]` CDP costs, PING lines, self-heal, watchdog | DSM `journalctl --user` is unreadable for user services — the file log is the only reliable one |
| RTSP camera still (OpenCV) | What the physical panel shows | Frame is often cropped (panel edges outside FOV); crop + upscale the region of interest; don't trust the vision model on this screen |

## Symptoms → causes

### Dashboard renders, but the config screen is rotated/mangled
The LVGL rotation mapping and the video-path rotation disagree. Check
`browser.c` flush: `lv_draw_sw_rotate` direction + the corner mapping
logical `(px,py)` → fb `(py, 1279−px)` must match `remote.c`. LVGL needs
`lv_display_set_rotation(ROTATION_90)` **and** the manual flush rotation
(PARTIAL mode).

### Black screen / garbage after a firmware change
Almost always the DSI init path was touched. `main.c`'s display path and the
`jc8012` base sequence are proven — restore them from git and bisect your
other changes.

### Panel shows stale colors after a CPU write to the framebuffer
Data cache not flushed before DMA2D read: `esp_cache_msync` (used by `/fill`).

### Tap latency: uniform ~1–1.7 s on every tap
On a dashboard that ticks (clock, auto-refresh): the **animation guard**
(`RENDERER_MIN_SEND_GAP_MS`) is permanently engaged because the change-streak
never drops below 3, queuing tap-result frames behind the gap. Verify:
renderer log sends exactly ~1 frame/s while the page changes; board
`frame_age` ≈ 1 s. Fix: the 3 s input-priority window in `renderer.js
input()` (watch for `input-bypass:` log lines after a tap). If latency is
bimodal (some taps instant, some ~1 s+), it is *not* the guard — check the
page's JS/CDN (dead widget request) or Homey API response times.

### Tap latency: nothing happens, screen looks frozen
Check in order: (1) board `/state` `input_sent` incrementing on taps;
(2) renderer log `[TOUCH-IN]` lines (board→CDP alive?); (3) renderer `cap=`
climbing (screenshot-bound loop → effective fps = 1/capMs); (4) board
`pong_age` growing (WS half-dead → lwIP, below).

### Board lwIP degrades under sustained animation
Symptom: after ~15–20 min of continuous frames, pongs stop, frames stall.
Cause: sustained ~2.5+ fps of 3 MB-equivalent traffic overwhelms the P4's
lwIP. Mitigations (in production): JPEG transport (not raw),
`RENDERER_MIN_SEND_GAP_MS=1000` animation guard, and the input-priority
window (keeps taps responsive without lifting the sustained cap). Do **not**
simply raise FPS or remove the guard to "fix" perceived sluggishness.
Tested boundary (2026-11-04): 500 ms (2 fps) under real sustained ~1.5 fps
page animation produced a near-freeze after ~10 min (50 s pong gap, 24 s
frame stall, backlog +37/5min) then self-recovered; 1000 ms (1 fps) shows
the backlog shrinking. The board's decode pipeline sustains ~1.5 fps under
load — 1 fps is the safe operating point with margin.

### Renderer "crashes" — but node never crashed
Forensics (the `crash1` episode): three service-level kills within 6 minutes,
node's own logs clean — an external actor (DSM housekeeping / another
service) killed the unit. The watchdog + `Restart=always` recover it; the
kill's source is fingerprinted via the watchdog log (`WATCHDOG_LOG`) when the
next one lands. Companion failure class: **page hang** — screenshot timeouts
×60+, stale frame resends, board `frame_age` frozen for minutes; a hung
page (JS deadlock / dead CDN request) with no recovery. Fix deployed:
self-heal page reload after `RENDERER_SHOT_RELOAD_AFTER` (8) consecutive
timeouts — verify on next hang by the `reloading page (self-heal)` log line.

### Wi-Fi drops after a firmware flash / C6 re-init
Brief C6 re-init on boot can drop the STA for a few seconds; the board
recovers (watch `/state` come back). Persistent drop: check the embedded C6
firmware version matches `esp_hosted` (2.12.12 ↔ `c6_fw_2_12_12.bin`).

### Board IP unknown / unreachable
It is DHCP on the home LAN. Reference deployment: `192.168.3.129`. If the
NAS is on a different subnet, scan for port 80 or use the serial console
(Wi-Fi IP is printed at boot).

## Environment gotchas

- **Synology DSM user services:** `XDG_RUNTIME_DIR=/run/user/<uid>` required;
  user `journalctl` empty (see build-and-flash.md); scratch dirs under
  `/volume1/hermes/cache/` are pruned after 24 h — keep persistent tools
  outside it.
- **`/tmp` on the NAS is noexec** — copy any downloaded binary to a
  `/volume1/` path before executing.
- **Mac SSH from the NAS:** use the paramiko venv + `tools/macssh.py`
  (credentials via env vars); the CH340 flash port is on the Mac.
- **esptool for the P4:** `--chip esp32p4`, 460800 baud works reliably;
  `--before default_reset --after hard_reset`.
