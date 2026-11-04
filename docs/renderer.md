# Renderer

Node.js service that renders a web page in headless Chromium and streams it to
the board. Dependencies: `puppeteer-core`, `ws`. Runs Node 22 with the
`chrome-headless-shell` binary (fetched by `fetch_chrome.py`).

## Process model

The systemd unit starts **`renderer_watchdog.py`**, not node directly. The
watchdog spawns `node renderer.js`, watches it, and restarts it on exit —
this is what survives the board's renderer-kill events (see
`troubleshooting.md` §crash1) and what `systemctl --user restart nas-renderer`
recreates.

## Capture loop

`renderer.js` runs a fixed-interval capture loop (slot = 1000 /
`RENDERER_FPS` ms, default 8 → 125 ms):

1. `page.screenshot()` of the 1280×800 clip (~160 ms on the NAS).
2. **Fingerprint:** downsample + hash. Compare with the previous frame.
   `changed = fingerprint differs beyond threshold`.
3. **Send gate:**
   - `changed` → send (subject to the animation guard, below).
   - unchanged → send only if the **heartbeat** elapsed
     (`RENDERER_HEARTBEAT_MS`, production value 5000 ms).
4. Frame sent as JPEG (`RENDERER_QUALITY`, production 70) over WebSocket with
   a sequence number.

Measured per-frame log line: `[renderer] cap=NNNms q=70 w=1280 sent=NNN fps=8`
(every ~5 s). `cap=` is screenshot+encode cost — if it climbs above ~300 ms
the effective frame rate drops to 1/cap and tap responsiveness degrades
proportionally.

## Animation guard and input bypass

The board's lwIP stack degrades under *sustained* frame load (measured: ~15–20
minutes of continuous ~2.5 fps before WebSocket pongs stop). The guard:

- `streak` = consecutive changed slots; `animating = streak >= 3`.
- While animating, sends are rate-limited to one per
  `RENDERER_MIN_SEND_GAP_MS` (production 1000 ms → 1 fps sustained).
- A static page never trips the guard (heartbeat only) and gets full
  8 fps tap-to-frame.

**The trap:** a dashboard that *ticks* every second (clock, auto-updating
widget) changes every capture slot too, so `streak` stays ≥ 3 and the guard is
permanently on — including for the frames that show the *result of a tap*.
Symptom: uniform ~1–1.7 s delay on every tap (not bimodal).

The fix (deployed): any touch event (`down`/`move`/`up` received from the
board) arms a **3-second input-priority window** during which the gap is
bypassed — tap results send immediately, while sustained animation stays
capped. Watch for `input-bypass: tap response sent during animation window`
in the log to confirm it fired.

## Self-heal (page hang)

A hung page (JS deadlock, dead CDN request) makes `page.screenshot()` time out
repeatedly (`RENDERER_SHOT_TIMEOUT_MS`) and the renderer keeps resending the
last frame — the board's `frame_age` freezes. Self-heal: after
`RENDERER_SHOT_RELOAD_AFTER` consecutive timeouts (production 8), the page is
reloaded in place and the streak counter reset. Log line:
`reloading page (self-heal)`.

## Touch injection

Board messages `{"type":"input","kind":"down|move|up","x":..,"y":..}` are
mapped to Chrome DevTools `Input.dispatchMouseEvent` (pressed/moved/released)
on the page at the given page coordinates. The board does the panel→page
coordinate conversion (it knows the physical geometry); the renderer just
injects. Per-event log line: `[TOUCH-IN] cdp=Nms` — the CDP injection cost
(~1–140 ms measured; not a latency concern).

## WebSocket protocol (board ⇄ renderer)

- Board connects out to `ws://<renderer>:9100` (renderer listens on `PORT`,
  production 9100).
- On connect the board sends its config: desired quality, page size, and the
  `url` it wants rendered (from its NVS endpoint settings; the service can
  pin a URL instead).
- Frames: JSON header + binary JPEG, with monotonically increasing `seq`.
- Renderer pings the board every 20 s (`PING`); the board's `/state` shows
  `pong_age` — if the board is connected but `pong_age` grows, the WS is
  half-dead (usually the board-side lwIP, see troubleshooting).

## Environment variables

| Var | Default | Production | Meaning |
|---|---|---|---|
| `PORT` | 9100 | 9100 | WS listen port (board connects here) |
| `PIN_URL` | — | `http://192.168.3.5:5050/viewer.html` | Fixed page to render (overrides board-supplied URL) |
| `RENDERER_WIDTH` / `RENDERER_HEIGHT` | 1280 / 800 | 1280 / 800 | Chrome window + capture clip (landscape standard) |
| `RENDERER_FPS` | 8 | 8 | Capture slot rate |
| `RENDERER_QUALITY` | 70 | 70 | JPEG quality (board asks for 30; 70 keeps text crisp on the LCD) |
| `RENDERER_MIN_SEND_GAP_MS` | 200 | 1000 | Sustained-animation send cap |
| `RENDERER_HEARTBEAT_MS` | 250 | 5000 | Forced send on a static page |
| `RENDERER_SHOT_TIMEOUT_MS` | — | set | Screenshot timeout |
| `RENDERER_SHOT_RELOAD_AFTER` | — | 8 | Consecutive timeouts before page reload (self-heal) |
| `RENDERER_SCREENCAST` | 0 | 0 | 1 = use Chrome screencast (event-driven) instead of one-shot captures |
| `RENDERER_KILL_ANIM` | — | — | Experimental animation kill-switch |
| `RENDERER_LOG` | — | `/tmp/renderer_nas.log` | Log file (DSM user journals are not readable from the shell — use this) |

## dashboard-server.js (optional)

A standalone, dependency-free Node server for a simple Homey on/off device
dashboard (the original pre-remote-render demo): serves `dashboard.html`,
`/api/devices` (onoff-capable devices) and `/api/toggle`. The Homey API token
comes from `HOMEY_TOKEN` in the environment — **never commit it**. The token
stays server-side; the browser/board never sees it.
