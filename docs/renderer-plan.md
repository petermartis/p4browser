# p4browser → full HTML+JS renderer for the Homey Pro dashboard

**Goal.** Run the Homey Pro automation dashboard (`http://192.168.3.5:5050/`, Dashboard
Studio app `com.satoer.dashboardstudio`) on the ESP32-P4's 10.1" MIPI-DSI touchscreen,
with the touch input driving the dashboard's own controls.

**Date.** 2026-10-12. Grounded by live probing of the dashboard, the board budget, and
the JC8012P4A1 touch hardware (see §1).

---

## 1. What we actually have (verified, not assumed)

### 1.1 The dashboard
- `GET /` → **753 B** `index.html` → loads `entry-router.js` → fetches `config.json`
  (507 B) + `settings.json` (43 B) → `window.location.replace` to **`viewer.html`**
  (`hideEditor:false` in config) or `editor.html`.
- `viewer.html` (5 KB) loads **~25 local JS modules** plus CDN deps:
  - `drivers/homey.js`, `drivers/mqtt.js`, `drivers/nodered.js`, `drivers/simulation.js`
  - `widget-base.js`, `widget-script-loader.js`, `widget-animations.js`, `formula-engine.js`
  - `templates.js`, `storage/web-provider.js`, `storage/local-provider.js`, `worker-timer.js`
  - **CDN:** `chart.js` (canvas 2D), `@phosphor-icons/web`, `markdown-it.min.js`, `purify.min.js`
  - **CSS:** `index.css`, `tailwind.generated.css`
- **Live data path:** `drivers/homey.js` opens a **Socket.IO** client
  (`io(...)`, loads `/socket.io/socket.io.js`) that connects to the **`:5050` server**,
  which holds the live socket to Homey and streams entity states.
  - Proven reachable from the NAS/board:
    `GET http://192.168.3.5:5050/socket.io/?EIO=4&transport=polling` → **200**,
    body `0{"sid":"...","upgrades":["websocket"],"pingInterval":25000,...}`.
  - **Homey itself (port 8000) is NOT reachable from the NAS** — the `:5050` server is
    the data source; the board never needs to talk to Homey directly.
- So "running the dashboard" = "running a browser that executes ~25 JS modules +
  chart.js + a Socket.IO client + a Web Worker, lays out Tailwind CSS, draws canvas
  charts, and renders SVG icons."

### 1.2 The board (build 45)
- SoC **ESP32-P4** (dual-core RISC-V, FPU), **16 MB flash**, **32 MB PSRAM (OCT 200 MHz)**.
- Flash layout (`partition.csv`): `ota_0` + `ota_1` = **2 × 4 MB (0x3F0000) app slots**,
  128 KB SPIFFS `storage`. Current app: **2.73 MB** → ~1.2 MB headroom in `ota_0`.
- Display: JD9365 MIPI-DSI, LVGL 9.3.0, working (Wikipedia renders, builds 43–45).
- **Touch present but not yet wired up** in firmware:
  - Controller **GSL3680 / Silead**, I2C addr **0x40**.
  - **SDA=GPIO7, SCL=GPIO8, RST=GPIO22, INT=GPIO21** (native 800×1280 coords).
  - LVGL transform for landscape 1280×800: `swap_xy=0, mirror_x=1, mirror_y=0`;
    disable internal I2C pullups.
  - A ready driver exists: **`mangoo1/esp_lcd_touch_gsl3680` v1.0.0** on the ESP
    Component Registry (implements the standard `esp_lcd_touch` interface).
  - Working JC8012P4A1 POC (display + touch + ESP-Hosted) is documented in
    `github.com/cptkirki/BETTA-HA-PANEL` issue #5 and
    `github.com/sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6`.

### 1.3 The NAS (the rendering host)
- x86_64, **16 GB RAM**, Node **22**, has internet, 19 TB free, runs the Hermes gateway.
- **No Chromium installed yet** (nothing on PATH). Installable (`npx @puppeteer/browsers`).

---

## 2. The core decision: two architectures

There is no "drop in a JS engine" shortcut. A real browser engine (V8 + Blink) will not
fit or run well on the P4 (2×4 MB app slots, RISC-V, no V8 port that fits). That leaves
two genuinely different end-states. **This is the decision you need to make.**

### Option A — "Native": run the actual Dashboard Studio SPA on the board
Build a self-contained HTML/CSS/JS runtime **on the ESP32-P4** that executes the
dashboard's real JS and renders it locally.

What it requires, minimally:
1. **JS engine** that fits: QuickJS (~300 KB) or Duktape. *Not* V8.
2. **DOM + a CSS layout engine** that understands the dashboard's Tailwind
   `tailwind.generated.css` + `index.css` (flexbox/grid, positioning) → maps to LVGL.
3. **Canvas 2D** implementation (for chart.js) — this is the hardest part; chart.js
   uses a large canvas 2D API surface.
4. **SVG** rendering for Phosphor icons.
5. **Socket.IO client** (polling + websocket upgrade) → to `:5050` (already reachable).
6. **Web Worker** support (`worker-timer.js`) — QuickJS can do in-thread workers but
   true off-thread workers need a second core + IPC.
7. **`fetch` / `XMLHttpRequest`** to the `:5050` asset + API endpoints.
8. **Touch → DOM events**: GSL3680 → LVGL `lv_indev` → synthesize `pointerdown/up`
   dispatched to the right DOM element via hit-testing in the layout tree.

**Honest feasibility:** the parts (1), (5), (7), (8) are tractable. The parts
(2), (3), (4) are a **large, multi-week engineering project** — you are effectively
writing a subset browser that must be *compatible enough* for one specific, heavy,
third-party SPA whose code you don't control and that will change with app updates.
chart.js + Tailwind + Web Workers is near the top of "what a microbrowser can't do
well." Flash fits (2.73 MB + QuickJS + LVGL), but **RAM is the risk** (LVGL already
uses a big chunk of the 32 MB; a DOM + layout + canvas runtime on top is tight).

**Pros:** fully self-contained (no NAS dependency), low input→display latency, works
offline for the layout.
**Cons:** very high cost; fragile to dashboard/app updates; canvas + Web Worker are
genuine risk; you maintain a mini-browser forever.

### Option B — "Remote render" (recommended): real browser on the NAS, board is the screen
This is the proven pattern (Home Assistant "Remote WebView" does exactly this for HA
dashboards on ESP32-S3 screens). A headless Chromium on the NAS runs the *actual*
Dashboard Studio SPA (full DOM, real chart.js, real Socket.IO to Homey, real Tailwind).
The NAS sends the board only what it needs to **draw and react**:

- **Render:** headless Chromium at the board's viewport (1280×800). Stream changed
  regions as compressed **JPEG/WebP tiles** (or full-frame on change) over a **WebSocket**
  to the board. Board decodes (JPEG in PSRAM) and blits via the existing LVGL/DMA2D
  flush path (already proven in builds 43–45).
- **Control:** GSL3680 touch → LVGL indev → board sends **input events over the same
  WebSocket** → NAS injects them into Chromium via **CDP `Input.dispatchMouseEvent`**
  (and `dispatchTouchEvent`/`Input.dispatchKeyEvent`). Because the events hit the *real*
  DOM, the dashboard's own click/toggle/switch handlers fire exactly as in a browser —
  **no dashboard logic is reimplemented at all.**
- **State sync (optional, cheaper than pixels):** for crisp controls, subscribe the
  NAS renderer to the Socket.IO state stream and push *semantic* state (which switches
  are on, values) to the board; the board can render controls as native LVGL widgets
  and only fall back to the JPEG for the rest. Hybrid = crisp interactive controls +
  pixel-accurate charts.

**Honest feasibility:** high. Every hard part (JS, DOM, layout, canvas, Socket.IO,
CSS) runs on the x86_64 NAS with a real engine — that part is *solved*. The board-side
work is: (a) GSL3680 indev, (b) a WebSocket client, (c) JPEG decode + blit, (d) a CDP
event forwarder on the NAS. All four are well-bounded. The board already has the
WebSocket-ish HTTP stack, LVGL flush, and PSRAM.

**Pros:** renders the real dashboard *exactly* (including chart.js, Tailwind, workers);
robust to app updates; board is cheap to build; control path uses the real DOM.
**Cons:** needs the NAS up (it's your always-on host anyway); input→display round-trip
adds ~30–80 ms (fine for toggling switches, not for 60 fps drag); one-time Chromium
install + a small renderer service.

---

## 3. Recommendation

**Build Option B first.** It delivers a *working, faithful* Homey dashboard on the
touchscreen in a bounded amount of work, and it is the only path that realistically
handles chart.js + Tailwind + Web Workers without a months-long mini-browser project.
Option A is a stretch/parallel research track — worth a spike (QuickJS + a static
HTML subset + GSL3680 indev) to de-risk, but do not block the dashboard on it.

The touch work is **shared** by both options (GSL3680 → LVGL indev), so it is Phase 1
either way.

---

## 4. Phase plan

### Phase 0 — Touch bring-up (shared, ~1–2 days)
- Add `mangoo1/esp_lcd_touch_gsl3680` via `idf_component.yml` (I2C 0x40, SDA GPIO7,
  SCL GPIO8, RST GPIO22, INT GPIO21, internal pullups off).
- Register an `lv_indev` with the transform `swap_xy=0, mirror_x=1, mirror_y=0`;
  feed native 800×1280 coords.
- **Verify:** on the existing Wikipedia screen, show a crosshair at the touch point and
  confirm it tracks correctly in 1280×800 landscape. Camera-check it.
- Deliverable: a `/touch` debug endpoint + camera-verified tracking.

### Phase 1 — Remote-render MVP (Option B, ~1–2 weeks)
**NAS side (renderer service, Node 22):**
- Install Chromium headless (`npx @puppeteer/browsers install chrome@stable`), drive
  with Puppeteer/CDP.
- Open `http://192.168.3.5:5050/viewer.html` at `1280×800`, `deviceScaleFactor=1`.
- Capture: on `page` change / interval (e.g. 4 Hz) send a full-frame **WebP/JPEG**
  (quality ~70) over a **WebSocket** to the board. Later: delta-tile diffing.
- Input: receive JSON `{type:'pointer', x, y, down, up}` from the board → CDP
  `Input.dispatchMouseEvent`/`dispatchTouchEvent`.
**Board side:**
- WebSocket client (lwIP WS) to the NAS renderer.
- On frame: decode JPEG/WebP into PSRAM, push to LVGL via the proven flush path.
- On touch: forward Phase-0 indev coords to the NAS.
- **Verify:** toggle a real switch/flow on the dashboard by touching the board; watch
  the state change (camera + Homey). Acceptance: a Homey device toggles end-to-end.

### Phase 2 — Crisp controls (hybrid, ~1 week, optional)
- NAS subscribes to the Socket.IO state stream; pushes semantic state to the board.
- Board renders switches/values/labels as **native LVGL** widgets (crisp, instant),
  overlays the JPEG only for chart/canvas areas. Big UX + latency win for controls.

### Phase 3 — Option A spike (research, parallel, unbounded)
- Spike: QuickJS on P4 + a *static* HTML subset (no canvas, no worker) + GSL3680 indev.
- Measure RAM/flash and where it breaks. Only proceed to full Option A if the spike
  shows a credible path **and** you want a NAS-independent board.

---

## 5. Risks / open questions
- **Input latency** (Option B): ~30–80 ms round-trip. Acceptable for toggles; if a
  widget needs drag/scrub, use Phase-2 native widgets for it.
- **NAS single point of failure:** renderer goes down → board shows last frame. Mitigate
  with a heartbeat + "renderer offline" LVGL overlay (the board's existing error path).
- **Homey is not directly reachable** from the NAS/board — the `:5050` server is the
  data source; if that server is down, the dashboard has no live data regardless of
  architecture. (Verify `:5050` health in Phase 0.)
- **RAM (Option A):** 32 MB PSRAM is already partly committed to LVGL; a DOM+canvas
  runtime may not fit. This is the main reason A is a spike, not the plan.
- **GSL3680 driver maturity:** the registry component is v1.0.0; the BETTA-HA-PANEL POC
  confirms the exact GPIOs/transform. Fallback: vendor the driver from the POC.

## 6. Acceptance (end-to-end)
1. Touch tracking camera-verified on the board (Phase 0).
2. The real Homey dashboard visible on the 10.1" screen with live values (Phase 1).
3. Toggling a Homey device **by touch on the board** changes the device state in Homey
   (Phase 1) — this is the headline: the touchscreen controls the dashboard.
4. (Optional) Crisp native controls (Phase 2).

## 7. Reuse of existing, working code
- MIPI-DSI/LVGL init + flush (builds 42–45) — **do not touch**; Option B reuses the
  exact flush path for JPEG frames.
- `esp_http_client` fetch + redirect handling (build 44) — reuse for asset fetch if
  Option A proceeds.
- `/state`, `/url?u=` HTTP debug endpoints — extend with `/touch` (Phase 0) and
  renderer heartbeat.
