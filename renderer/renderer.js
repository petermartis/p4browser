#!/usr/bin/env node
/*
 * renderer.js — Option B remote-render service (NAS-side).
 *
 * Runs a headless Chrome (chrome-headless-shell), loads the Homey Dashboard
 * Studio SPA, and streams rendered frames (JPEG) to the ESP32-P4 board over a
 * WebSocket. The board's touchscreen input is injected back into the real
 * page via CDP, so the dashboard's own JS handlers fire natively.
 *
 * Wire protocol (JSON text frames), board -> service:
 *   { "type":"connect", "url":"http://192.168.3.5:5050/",
 *     "width":1280, "height":800, "fps":12, "quality":60 }
 *   { "type":"navigate", "url":"..." }
 *   { "type":"input", "kind":"down|move|up|wheel",
 *     "x":.., "y":.., "button":"left"|"middle"|"right"|"none",
 *     "dx":.., "dy":.., "timestamp":<ms> }
 *   { "type":"ping" }
 *
 * service -> board:
 *   { "type":"hello", "seq":0, "ready":true }
 *   { "type":"frame", "seq":N, "w":1280, "h":800, "ts":<page ms>,
 *     "jpeg":"<base64>" }
 *   { "type":"nav",  "ok":true, "url":"...", "title":"..." }
 *   { "type":"err",  "msg":"..." }
 *   { "type":"pong" }
 *
 * Only changed frames are sent (cheap perceptual diff on downsampled JPEG).
 *
 * Run:  LD_LIBRARY_PATH=/volume1/hermes/renderer/lib:/usr/lib:/lib \
 *       node /volume1/hermes/renderer/renderer.js [--port 9100]
 */
'use strict';

const path = require('path');
const fs = require('fs');
const { WebSocketServer, WebSocket } = require('ws');
const puppeteer = require('puppeteer-core');

// Host-configurable Chrome: NAS default is its x86_64 portable build; the
// Pi5 override sets CHROME_PATH=/usr/bin/chromium (system ARM64 Chromium).
const CHROME = process.env.CHROME_PATH || '/volume1/hermes/renderer/chrome/chrome-linux64/chrome-linux64/chrome';
const PORT = (parseInt(process.argv.find(a => a === '--port') ? process.argv[process.argv.indexOf('--port') + 1] : '9100', 10));
const DEFAULT_URL = 'http://192.168.3.5:5050/';
// Optional pin: force a specific render target regardless of the board's
// connect url (set via PIN_URL env in the service). Lets us point the board at
// a custom dashboard (e.g. the real-device control page) without a board rebuild.
const PIN_URL = process.env.PIN_URL || '';

// Perceptual diff: downsample to this width, compare mean abs byte diff.
const DIFF_W = 64;
const DIFF_THRESHOLD = 1.5; // avg abs byte diff over downsampled JPEG (~0..255)

async function launch() {
  // Host-specific fontconfig + shared libs. The NAS needs its minimal
  // fonts.conf + bundled lib dir (DSM's fontconfig segfaults on complex match
  // rules). The Pi5 (Debian bookworm) uses the system defaults — neither NAS
  // path exists there, so we only override when the host's own files are
  // actually present (or the env explicitly sets them).
  const NAS_FONTCONF = '/volume1/hermes/renderer/fontconf/fonts.conf';
  const NAS_LIB = '/volume1/hermes/renderer/lib';
  const env = { ...process.env };
  if (process.env.FONTCONFIG_FILE || fs.existsSync(NAS_FONTCONF)) {
    env.FONTCONFIG_FILE = process.env.FONTCONFIG_FILE || NAS_FONTCONF;
    env.FONTCONFIG_PATH = process.env.FONTCONFIG_PATH || path.dirname(NAS_FONTCONF);
  }
  if (process.env.LD_LIBRARY_PATH || fs.existsSync(NAS_LIB)) {
    env.LD_LIBRARY_PATH = process.env.LD_LIBRARY_PATH || (NAS_LIB + ':/usr/lib:/lib');
  }
  return puppeteer.launch({
    executablePath: CHROME,
    headless: true, // full Chrome new-headless (the headless-shell crashes on
                    // the Homey SPA's font/render workload)
    env,
    args: [
      '--no-sandbox',
      '--disable-setuid-sandbox',
      '--disable-dev-shm-usage',
      '--disable-gpu',
      '--hide-scrollbars',
      '--force-color-profile=srgb',
      '--font-render-hinting=none',
      '--mute-audio',
      '--window-size=1280,800',
    ],
    dumpio: false,
  });
}

// Coarse change-detection: sample a fixed grid of bytes from the JPEG (the
// SOI/header bytes are constant, so we skip them) and keep their values. A
// redraw shifts the compressed payload enough that the sampled mean-abs-diff
// exceeds DIFF_THRESHOLD. We never decode pixels — this is a cheap "did it
// change" signal, not a comparison.
const FINGER_N = 256;      // number of sampled bytes
const FINGER_SKIP = 256;   // skip the constant JPEG header region

function fingerprint(buf) {
  const body = buf.subarray(Math.min(FINGER_SKIP, buf.length));
  const step = Math.max(1, Math.floor(body.length / FINGER_N));
  const fp = new Int8Array(FINGER_N);
  let n = 0;
  for (let i = 0; i < body.length && n < FINGER_N; i += step) fp[n++] = body[i] | 0;
  return { len: buf.length, fp, n };
}

function fpDiff(a, b) {
  if (a.len !== b.len) return 255; // payload size changed => redrawn
  const n = Math.min(a.n, b.n);
  let s = 0;
  for (let i = 0; i < n; i++) s += Math.abs(a.fp[i] - b.fp[i]);
  return n ? s / n : 0;
}

// --- Persistent render session -------------------------------------------
// `active` is the single Client that owns the live Chrome + page + screencast.
// It survives board WebSocket reconnects: when the board's WS drops and
// reconnects, we RE-ATTACH the new WS to `active` (adopting its session)
// instead of killing Chrome and launching a new one. That is what used to
// "tear down" the renderer on every board reconnect (full Chrome + page
// reload + screencast restart = a blank flash on the 2 GB Pi5).
//
// `pushFrame` is the single choke-point for sending a rendered frame to the
// board. It always reads `active.ws` at send time, so the screencast callback
// (which is bound to whichever Client started the screencast) keeps streaming
// to the *current* board even after that WS has been swapped out on reconnect.
let active = null;
const SESSION_GRACE_MS = 60000; // keep Chrome after the board WS drops this long
let clientSeq = 0;              // monotonic id per WS client (diagnostics)

function pushFrame(jpeg) {
  const c = active;
  if (!c || !c.ws || c.ws.readyState !== 1) return;
  c.seq++;
  c.framesSent++;
  c._lastJpeg = jpeg;
  c._lastFrameMs = Date.now();
  c.send({ type: 'frame', seq: c.seq, w: c.width, h: c.height, jpeg });
}

class Client {
  constructor(ws) {
    this.ws = ws;
    this._id = ++clientSeq;  // diagnostics: which physical WS does a msg go to/from
    this.browser = null;
    this.page = null;
    this.loop = null;
    this.seq = 0;
    this.fps = 12;
    this.quality = 85;
    this.width = 1280;
    this.height = 800;
    this.rotateToPage = false; // true when board (800x1280 portrait) feeds a landscape page
    this.mouse = { x: 0, y: 0, down: false };
    this.lastFp = null;
    this.mouseTs = 0;
    this.framesSent = 0;        // total frames pushed to this client (diag)
    this.browserUp = false;     // true once connect() launched Chrome (diag)
    this.connectedAt = Date.now();
    this.lastMsgAt = Date.now(); // last inbound message (idle-timeout sweeper)
    this.tick = null;            // periodic liveness tick interval (startTick)
  }

  send(obj) {
    if (this.ws.readyState === 1) this.ws.send(JSON.stringify(obj));
  }

  // The board force-reconnects if it hears NOTHING (any type) for 30s — that's
  // how it detects a half-open socket (it can still send pings, but the
  // renderer->board direction is dead). The capture loop only sends frames on
  // change, so a static dashboard would go silent and trigger a false
  // reconnect. This tick (every 5s, only once Chrome is up) guarantees the
  // board hears from us regularly. Cheap: a 13-byte JSON text frame.
  startTick() {
    if (this.tick) return;
    this.tick = setInterval(() => {
      // ONLY the current session owner sends ticks/keepalive frames. Zombie
      // clients (board sockets whose FIN never arrived — lwIP defers the FIN
      // while unread RX data sits in the abandoned buffer) keep this interval
      // running forever; addressing `active` from every one of them flooded
      // the LIVE board socket with ticks plus 100KB keepalive frames every 5s
      // (the retransmit below doesn't refresh _lastFrameMs, so each zombie
      // re-sent the full frame). That flood starved pongs on the link ->
      // board pong-timeout -> reconnect -> more zombies. Ownership gate makes
      // a zombie's interval a no-op.
      if (active !== this) return;
      const c = this;
      if (!c.ws || c.ws.readyState !== 1) return;
      c.send({ type: 'tick' });
      // DEBUG: rate-limited (20s) keepalive diagnostic.
      if (Date.now() - (c._tickLogAt || 0) > 20000) {
        c._tickLogAt = Date.now();
        const age = c._lastFrameMs ? ((Date.now() - c._lastFrameMs) / 1000).toFixed(0) + 's' : 'never';
        console.log(`[renderer] tick: client#${c._id} lastJpeg=${!!c._lastJpeg} lastFrameAge=${age} wsReady=${c.ws.readyState} seq=${c.seq} framesSent=${c.framesSent}`);
      }
      // Keepalive frame: the board force-reconnects if it gets NO FRAME for
      // 30s (separate from the no-message tick check). On a static dashboard
      // the raw-CDP screencast only emits on change, so it goes silent and the
      // board's no-frame check fires a false reconnect. If the last real frame
      // is >15s old, re-send it (fresh seq) so the board decodes the unchanged
      // frame and stays connected. Cheap: one re-encoded-free JPEG blit.
      if (c._lastJpeg && Date.now() - (c._lastFrameMs || 0) > 15000) {
        c.seq++;
        c.send({ type: 'frame', seq: c.seq, w: c.width, h: c.height, jpeg: c._lastJpeg });
      }
    }, 5000);
  }

  async robustGoto(url, timeout = 30000) {
    // Homey's SPA does client-side redirects that can detach the initial frame
    // mid-navigation ("Navigating frame was detached"). Use domcontentloaded
    // (not networkidle2, which never settles on a Socket.IO SPA) and swallow
    // the detached-frame race — the page keeps loading in the background.
    try {
      await this.page.goto(url, { waitUntil: 'domcontentloaded', timeout });
    } catch (e) {
      const m = String(e && e.message || e);
      if (!/detached|navigated|navigation|interrupted/i.test(m)) throw e;
    }
    // Give the SPA a beat to hydrate before we start streaming.
    await new Promise((r) => setTimeout(r, 1500));
  }

  async connect(cfg) {
    try {
      await this.shutdown();
      this.fps = parseInt(process.env.RENDERER_FPS, 10) || cfg.fps || 12;
      // FPS TUNING: quality is env-overridable (RENDERER_QUALITY) so we can
      // retest without a re-push; falls back to the board's requested value.
      this.quality = parseInt(process.env.RENDERER_QUALITY, 10) || (cfg.quality || 60);
      // FPS TUNING: width/height env-overridable (RENDERER_WIDTH/HEIGHT) so we
      // can retest a lower render resolution without a re-push. Lower res cuts
      // the Homey SPA's recomposite cost (the one-shot capture is ~4.5s at
      // 1280x800 because the SPA is continuously re-compositing).
      this.width = parseInt(process.env.RENDERER_WIDTH, 10) || cfg.width || 1280;
      this.height = parseInt(process.env.RENDERER_HEIGHT, 10) || cfg.height || 800;
      // Board reports its display geometry. If it's portrait (H>W) while the
      // page viewport is landscape (W>=H), the board rotates frames 90deg to
      // present them — so touch coords must be rotated the same way here.
      this.rotateToPage = !!(cfg.bw && cfg.bh && cfg.bh > cfg.bw && this.width >= this.height);
      this.browser = await launch();
      this.browserUp = true;
      this.page = await this.browser.newPage();
      await this.page.setViewport({ width: this.width, height: this.height });
      await this.robustGoto(PIN_URL || cfg.url || DEFAULT_URL);
      // FPS TUNING: kill CSS animations + transitions so the Chromium renderer
      // stops continuously re-compositing (it was pegging ~125% CPU on the Pi5,
      // starving page.screenshot() and capping the frame rate to ~1/4.5s).
      // Re-applied on every (re)connect; JS-driven updates (clocks, widget
      // refreshes) still render, but the idle animation churn is gone.
      if (parseInt(process.env.RENDERER_KILL_ANIM, 10) !== 0) {
        try {
          await this.page.addStyleTag({ content:
            '*,*::before,*::after{animation-duration:0.0001ms!important;' +
            'animation-iteration-count:1!important;transition-duration:0.0001ms!important;}'
          });
        } catch (_) {}
      }
      this.seq = 0;
      this.lastFp = null;
      await this.send({ type: 'hello', seq: 0, ready: true,
        title: await this.page.title().catch(() => ''),
        url: this.page.url() });
      await this.send({ type: 'nav', ok: true, url: this.page.url(),
        title: await this.page.title().catch(() => '') });
      this.startLoop();
      this.startTick();
    } catch (e) {
      await this.send({ type: 'err', msg: String(e && e.message || e) });
    }
  }

  async navigate(url) {
    if (!this.page) return;
    try {
      await this.robustGoto(url);
      this.lastFp = null;
      await this.send({ type: 'nav', ok: true, url: this.page.url(),
        title: await this.page.title().catch(() => '') });
    } catch (e) {
      await this.send({ type: 'nav', ok: false, url, msg: String(e && e.message || e) });
    }
  }

  // On-demand reload: re-fetch the pinned page (fresh from Homey) WITHOUT
  // restarting Chrome — so a dashboard change shows up on the board with no
  // board reboot. Uses the same robustGoto path as connect/navigate, so it
  // handles the Homey SPA redirect race. Falls back to the current URL when
  // no PIN_URL is set.
  async reload() {
    if (!this.page) return;
    const url = PIN_URL || this.page.url();
    console.log(`[renderer] reload -> ${url}`);
    try {
      await this.robustGoto(url);
      this.lastFp = null;
      await this.send({ type: 'nav', ok: true, url: this.page.url(),
        title: await this.page.title().catch(() => '') });
      console.log(`[renderer] reload done: ${this.page.url()}`);
    } catch (e) {
      await this.send({ type: 'nav', ok: false, url, msg: String(e && e.message || e) });
    }
  }

  startLoop() {
    if (this.loop) return;
    console.log(`[renderer] startLoop: SCREENCAST=${JSON.stringify(process.env.RENDERER_SCREENCAST)} fps=${this.fps} page=${!!this.page} url=${this.page && this.page.url()}`);
    // Streaming screencast (Page.startScreencast) — grabs compositor frames as
    // they land instead of requesting a fresh render. Much faster than the
    // one-shot Page.captureScreenshot when the page is continuously
    // re-compositing (the Homey SPA re-renders its widgets, which starves the
    // one-shot capture and caps it at ~1/4.5s). Enabled via RENDERER_SCREENCAST=1.
    if (process.env.RENDERER_SCREENCAST === '1') {
      this.loop = 'screencast'; // sentinel: blocks re-entry (clearTimeout on a string is a no-op)
      this._startScreencast();
      return;
    }
    this._fallbackCaptureLoop();
  }

  // One-shot capture loop (recursive setTimeout, non-overlapping). This is the
  // default path, and the fallback if the screencast fails to start.
  _fallbackCaptureLoop() {
    if (this.loop) return;
    const interval = () => Math.max(16, Math.round(1000 / this.fps));
    const tick = async () => {
      const t0 = Date.now();
      try {
        if (this.page) {
          // A screenshot that never settles (hung heavy SPA / saturated CDP)
          // used to kill this loop forever: the await never resolves, no
          // catch, no reschedule -> board freezes on the last frame. Race it
          // against a timeout so a hung page loses one slot, not the loop.
          const shot = await Promise.race([
            this.page.screenshot({
              type: 'jpeg', quality: this.quality,
              clip: { x: 0, y: 0, width: this.width, height: this.height },
            }),
            new Promise((_, reject) => setTimeout(
              () => reject(new Error('screenshot timeout')),
              parseInt(process.env.RENDERER_SHOT_TIMEOUT_MS, 10) || 3000)),
          ]);
          const capMs = Date.now() - t0;
          this._shotStreak = 0; // capture recovered — clear the self-heal streak
          const fp = fingerprint(shot);
          const changed = !this.lastFp || fpDiff(this.lastFp, fp) > DIFF_THRESHOLD;
          this.lastFp = fp;
          // Guaranteed refresh: the coarse fingerprint can miss a small change
          // (a single tile toggle is only ~6% of the frame). Force a frame at
          // least every RENDERER_HEARTBEAT_MS (default 250ms) so the board
          // never stays on a stale image.
          const hbMs = parseInt(process.env.RENDERER_HEARTBEAT_MS, 10) || 250;
          const heartbeat = (Date.now() - (this._lastFrameMs || 0)) > hbMs;
          // Sustained-load guard, streak-based: only a page that changes
          // EVERY slot (continuous animation, like the demo) is capped at
          // RENDERER_MIN_SEND_GAP_MS (service: 1000ms = 1/s) — a sustained
          // stream drowns the board's lwIP (pong starvation -> npong flap
          // -> touch starvation -> FD-leak degradation). A tap on a static
          // page changes the DOM for 1-2 slots (streak < 3) -> no gap ->
          // full fps responsiveness, even for rapid successive taps.
          if (changed) this._streak = (this._streak || 0) + 1; else this._streak = 0;
          const animating = this._streak >= 3;
          const minGapMs = parseInt(process.env.RENDERER_MIN_SEND_GAP_MS, 10) || 200;
          const bypass = Date.now() < (this._inputBypassUntil || 0);
          const gapOk = !animating || bypass || (Date.now() - (this._lastSendMs || 0)) >= minGapMs;
          if ((changed || heartbeat) && (heartbeat || gapOk)) {
            if (animating && bypass && Date.now() - (this._bypassLogAt || 0) > 2000) {
              this._bypassLogAt = Date.now();
              console.log(`[renderer] input-bypass: tap response sent during animation window`);
            }
            this.seq++;
            this.framesSent++;
            // Rate-limited capture-time diagnostic (every 5s): the real
            // per-frame cost on this host (screenshot + JPEG encode). If capMs
            // is ~1000/fps, the loop is capture-bound; if much larger, the
            // screenshot itself is the bottleneck (tune quality/size).
            if (Date.now() - (this._capLogAt || 0) > 5000) {
              this._capLogAt = Date.now();
              console.log(`[renderer] cap=${capMs}ms q=${this.quality} w=${this.width} sent=${this.framesSent} fps=${this.fps}`);
            }
            // puppeteer may hand back a plain Uint8Array (not a Buffer) — a
            // Uint8Array.toString('base64') ignores the arg and yields comma-
            // joined decimals, which the board's base64 decoder mangles. Wrap in
            // Buffer so .toString('base64') is always real base64.
            const jpeg = Buffer.from(shot).toString('base64');
            this._lastJpeg = jpeg;
            this._lastFrameMs = Date.now();
            this._lastSendMs = Date.now();
            this.send({ type: 'frame', seq: this.seq, w: this.width, h: this.height,
              jpeg });
          }
        }
      } catch (e) { /* page navigated/closed; loop keeps retrying */
        if (Date.now() - (this._shotErrLogAt || 0) > 10000) {
          this._shotErrLogAt = Date.now();
          console.log(`[renderer] capture error: ${e.message} (loop continues)`);
        }
        // Self-heal: a hung page (screenshot never settles — JS spin, wedged
        // compositor) fails EVERY slot, so the board sits on a stale frame
        // until its ~30 s no-frames watchdog flaps the WS. After N
        // consecutive screenshot timeouts, reload the page ONCE for a second
        // life. 2026-11-02: the Dashboard Studio viewer hung exactly like
        // this mid-session (60+ consecutive timeouts, only a renderer
        // restart recovered it).
        if (e.message === 'screenshot timeout') {
          this._shotStreak = (this._shotStreak || 0) + 1;
          const reloadAfter = parseInt(process.env.RENDERER_SHOT_RELOAD_AFTER, 10) || 8;
          if (this._shotStreak >= reloadAfter && !this._shotReloadPending) {
            const n = this._shotStreak;
            this._shotStreak = 0;
            this._shotReloadPending = true;
            console.log(`[renderer] ${n} consecutive screenshot timeouts — reloading page (self-heal)`);
            this.page.reload({ waitUntil: 'load', timeout: 15000 })
              .then(() => console.log('[renderer] self-heal: page reload done'))
              .catch((re) => console.log(`[renderer] self-heal: page reload failed: ${re.message}`))
              .finally(() => { this._shotReloadPending = false; });
          }
        }
      }
      // Non-overlapping: schedule the NEXT capture only AFTER this one
      // finished. The old setInterval fired every 1000/fps ms regardless of
      // whether the awaited screenshot had completed, so a slow capture piled
      // up overlapping screenshots and the real rate collapsed to
      // ~1/screenshot_time (the ~0.5 fps). Recursive setTimeout serializes.
      this.loop = setTimeout(tick, interval());
    };
    this.loop = setTimeout(tick, 0);
  }

  // Streaming screencast (Page.startScreencast) over a RAW CDP client — a
  // direct WS connection to the page target. Rationale: Puppeteer's
  // page.createCDPSession() hangs on this busy SPA, because it routes through
  // the browser's main CDP connection, which is saturated by the screenshot
  // loop + touch input (a new session through it never completes). A direct
  // WS to /devtools/page/<id> attaches instantly (verified by cdp_probe.js /
  // cdp_sc_test.js). The screencast captures compositor frames as they land
  // (~the page's real re-composite rate) instead of requesting a fresh ~4.5s
  // one-shot render, and makes touch response near-instant (new state appears
  // on the next composited frame, ~600ms, instead of ~5s).
  async _startScreencast() {
    const http = require('http');
    let cdpPort = null;
    try {
      const bws = this.browser.wsEndpoint(); // ws://127.0.0.1:PORT/devtools/browser/UUID
      const m = bws.match(/:([0-9]+)\//);
      cdpPort = m ? parseInt(m[1], 10) : null;
      if (!cdpPort) throw new Error('no cdp port in ' + bws);
      // Find the page target (match by URL) to get its direct WS endpoint.
      const list = await new Promise((resolve, reject) => {
        const req = http.get({ host: '127.0.0.1', port: cdpPort, path: '/json/list' },
          (res) => { let b = ''; res.on('data', (d) => b += d);
            res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } }); });
        req.on('error', reject); req.setTimeout(5000, () => req.destroy(new Error('json/list timeout')));
      });
      const cur = this.page && this.page.url();
      const target = list.find(t => t.type === 'page' && t.url === cur) ||
                     list.find(t => t.type === 'page' && t.url.includes('5050'));
      if (!target) throw new Error('page target not found for ' + cur);
      console.log(`[renderer] _startScreencast: raw CDP to ${target.webSocketDebuggerUrl}`);
      const ws = new WebSocket(target.webSocketDebuggerUrl);
      this._scWs = ws;
      let id = 0;
      const send = (method, params) => new Promise((resolve, reject) => {
        const myId = ++id;
        const onMsg = (data) => {
          const msg = JSON.parse(data.toString());
          if (msg.id === myId) { ws.off('message', onMsg); msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result); }
        };
        ws.on('message', onMsg);
        ws.send(JSON.stringify({ id: myId, method, params }));
        setTimeout(() => reject(new Error('cdp timeout ' + method)), 10000);
      });
      let t0 = Date.now();
      let frames = 0;
      ws.on('message', (data) => {
        let msg; try { msg = JSON.parse(data.toString()); } catch { return; }
        if (msg.method === 'Page.screencastFrame') {
          // pushFrame() reads `active` at send time, so after a board
          // reconnect (new WS attached to the same session) frames keep
          // flowing to the current board with no screencast restart.
          pushFrame(msg.params.data);
          // CDP requires acking with the frame's frameId (from metadata). The
          // old code sent { sessionId } (undefined on a raw page-WS), an empty
          // ack — Chromium never considered frames acknowledged and stopped
          // emitting after ~1 frame -> 0.01fps -> board stuck on a stale frame.
          ws.send(JSON.stringify({ id: ++id, method: 'Page.screencastFrameAck',
            params: { frameId: (msg.params.metadata && msg.params.metadata.frameId) || 0 } }));
          frames++;
          const el = (Date.now() - t0) / 1000;
          if (el >= 5) { console.log(`[renderer] screencast ${(frames / el).toFixed(2)}fps sent=${active ? active.framesSent : 0} q=${active ? active.quality : '?'}`); frames = 0; t0 = Date.now(); }
        }
      });
      ws.on('error', (e) => console.log(`[renderer] screencast ws error: ${e.message}`));
      ws.on('close', () => console.log(`[renderer] screencast ws closed`));
      await new Promise((resolve, reject) => {
        ws.on('open', resolve);
        const to = setTimeout(() => reject(new Error('sc ws open timeout')), 8000);
        ws.on('error', (e) => { clearTimeout(to); reject(e); });
      });
      await send('Page.enable', {});
      await send('Page.startScreencast', {
        format: 'jpeg', quality: this.quality,
        maxWidth: this.width, maxHeight: this.height,
        everyNthFrame: parseInt(process.env.RENDERER_SCREENCAST_N || '3', 10),
      });
      console.log(`[renderer] screencast STARTED (raw CDP, q=${this.quality} ${this.width}x${this.height})`);
    } catch (e) {
      console.log(`[renderer] screencast FAILED: ${e.message}`);
      if (this._scWs) { try { this._scWs.close(); } catch (_) {} }
      this._scWs = null;
      // Fall back to the one-shot capture loop so the board still gets frames.
      this.loop = null;
      this._fallbackCaptureLoop();
    }
  }

  async input(ev) {
    if (!this.page) return;
    const m = this.page.mouse;
    // Board touch coords are in its 800x1280 portrait memory space; the page
    // viewport is 1280x800 landscape. Rotate 90deg CW so the board's physical
    // orientation matches the page: pageX = touchY, pageY = 800 - touchX.
    const tx = ev.x | 0, ty = ev.y | 0;
    const x = this.rotateToPage ? ty : tx;
    const y = this.rotateToPage ? (this.height - tx) : ty;
    // 'none' is not a real CDP mouse button (it's the release convention);
    // map it to the button we went down with (default left).
    const btn = (ev.button && ev.button !== 'none') ? ev.button : 'left';
    this.mouseTs = Date.now();
    const tIn = this.mouseTs;
    // Input-priority window: the user just touched the screen — the page
    // will change shortly and those frames must NOT wait for the
    // sustained-animation send gap. A ticking dashboard (clock etc.) keeps
    // the change-streak >= 3, which would queue the tap result behind a
    // 1 s gap. A bounded 3 s fast window stays safe for the board's lwIP
    // (a tap burst is not a sustained stream).
    this._inputBypassUntil = Date.now() + 3000;
    if (ev.kind === 'down' || ev.kind === 'up')
      console.log(`[TOUCH-IN] ${new Date(tIn).toISOString()} kind=${ev.kind} board=(${tx},${ty}) page=(${x},${y}) btn=${btn}`);
    switch (ev.kind) {
      case 'down':
        this.mouse.down = true; this.mouse.x = x; this.mouse.y = y;
        await m.move(x, y);
        await m.down({ button: btn });
        break;
      case 'move':
        this.mouse.x = x; this.mouse.y = y;
        await m.move(x, y);
        break;
      case 'up':
        this.mouse.down = false;
        await m.move(x, y);
        await m.up({ button: btn });
        break;
      case 'wheel':
        await m.wheel({ deltaX: ev.dx | 0, deltaY: ev.dy | 0 });
        break;
    }
    if (ev.kind === 'down' || ev.kind === 'up')
      console.log(`[TOUCH-OUT] ${new Date().toISOString()} kind=${ev.kind} cdp=${Date.now() - tIn}ms`);
  }

  // Take over a donor's live session (Chrome + page + screencast) WITHOUT
  // restarting anything. The donor is a prior connection whose board WS closed
  // (flapping) but whose Chrome is still alive. After this, `this` is the
  // session owner: the running screencast (bound to the donor) pushes frames
  // via pushFrame() -> `active` (which the caller sets to `this`), so frames
  // flow to THIS client's board WS. No new Chrome, no page reload, no blank.
  adoptFrom(donor) {
    this.browser = donor.browser;
    this.browserUp = true;
    this.page = donor.page;
    this._scWs = donor._scWs;
    this.loop = donor.loop; // 'screencast' sentinel (or a fallback timeout)
    this.seq = donor.seq;
    this.framesSent = donor.framesSent;
    this.lastFp = donor.lastFp;
    this._lastJpeg = donor._lastJpeg;
    this._lastFrameMs = donor._lastFrameMs;
    this.quality = donor.quality;
    this.width = donor.width;
    this.height = donor.height;
    this.rotateToPage = donor.rotateToPage;
    this.mouse = donor.mouse;
    // The donor stops owning the session: drop its periodic handles.
    if (donor.tick) { clearInterval(donor.tick); donor.tick = null; }
    if (donor._graceTimer) { clearTimeout(donor._graceTimer); donor._graceTimer = null; }
    donor._dead = true;
    donor.browser = null; donor.page = null; donor._scWs = null; donor.browserUp = false;
  }

  // Re-attach path after adoptFrom(): no Chrome launch, no page load, no
  // screencast restart. Refresh geometry from the new board, resume the
  // keepalive tick, and tell the board it's ready. If the adopted screencast
  // WS died while the board was gone (page reloaded, CDP socket closed),
  // restart just the screencast — still no Chrome/page restart.
  async resume(cfg) {
    this.fps = parseInt(process.env.RENDERER_FPS, 10) || cfg.fps || this.fps;
    this.quality = parseInt(process.env.RENDERER_QUALITY, 10) || cfg.quality || this.quality;
    this.width = parseInt(process.env.RENDERER_WIDTH, 10) || cfg.width || this.width;
    this.height = parseInt(process.env.RENDERER_HEIGHT, 10) || cfg.height || this.height;
    this.rotateToPage = !!(cfg.bw && cfg.bh && cfg.bh > cfg.bw && this.width >= this.height);
    // In-page navigation (a tap on a link inside the page, e.g. the viewer's
    // "edit" button -> editor.html) drags the page away from the pinned
    // target, and an adopt keeps whatever URL the page is on. Re-pin on
    // adopt so the board always returns to the deployment target — the
    // editor SPA in particular hangs the capture loop.
    if (PIN_URL && this.page && !this.page.url().startsWith(PIN_URL)) {
      try {
        await this.robustGoto(PIN_URL);
        console.log(`[renderer] adopt: re-pinned to ${PIN_URL}`);
      } catch (e) {
        console.log(`[renderer] adopt re-pin failed: ${e.message}`);
      }
    }
    this.connectedAt = Date.now();
    this.lastMsgAt = Date.now();
    this.startTick();
    // The adopted donor's capture loop (this.loop) is bound to the DONOR's
    // `this`: its recursive setTimeout closes over the donor, so every frame
    // it captures is sent to the dead donor, never to THIS client's board WS.
    // That is why the screen froze on reconnect (only the 5s keepalive
    // re-sent the stale JPEG). Clear the donor's loop + any stale screencast
    // WS and start a FRESH capture loop on this client, honouring
    // RENDERER_SCREENCAST (the old `scOk` check probed _scWs, which the
    // fallback loop never sets, so it always fell through to
    // _startScreencast() — the unreliable CDP path).
    const inheritedLoop = this.loop;
    if (inheritedLoop && inheritedLoop !== 'screencast') clearTimeout(inheritedLoop);
    if (this._scWs) { try { this._scWs.removeAllListeners(); this._scWs.close(); } catch (_) {} this._scWs = null; }
    this.loop = null;
    if (inheritedLoop) {
      console.log(`[renderer] adopted session — restarting capture loop on client#${this._id} (RENDERER_SCREENCAST=${process.env.RENDERER_SCREENCAST})`);
      this.startLoop();
    }
    await this.send({ type: 'hello', seq: this.seq, ready: true,
      title: await this.page.title().catch(() => ''),
      url: this.page.url() });
    await this.send({ type: 'nav', ok: true, url: this.page.url(),
      title: await this.page.title().catch(() => '') });
  }

  async shutdown() {
    if (this.loop) { clearTimeout(this.loop); this.loop = null; }
    if (this.tick) { clearInterval(this.tick); this.tick = null; }
    if (this._scWs) { try { this._scWs.removeAllListeners(); this._scWs.close(); } catch (_) {} this._scWs = null; }
    try { if (this.browser) await this.browser.close(); } catch (_) {}
    this.browser = null; this.page = null;
  }
}

const wss = new WebSocketServer({ host: '0.0.0.0', port: PORT });
console.log('[renderer] WS server on ws://0.0.0.0:' + PORT);

// Global registry of all live clients — lets a `diag` query report on every
// connected client (e.g. the board's frame count) even when the query comes
// from a different (probe) client.
const clients = new Set();

wss.on('connection', (ws) => {
  const c = new Client(ws);
  clients.add(c);
  console.log(`[renderer] client#${c._id} connected`);
  ws.on('message', async (data) => {
    let msg;
    try { msg = JSON.parse(data.toString()); } catch { return; }
    c.lastMsgAt = Date.now();   // any inbound message counts as "alive"
    try {
      switch (msg.type) {
        case 'connect': {
          // Single-board design: exactly one session owns the Chrome. On a
          // board (re)connect, if a live session already exists (a prior
          // connection whose board WS dropped — flapping — but whose Chrome
          // survived the grace period), ADOPT it: hand its Chrome + page +
          // running screencast to this client WITHOUT restarting them. This is
          // the fix for the "renderer tearing down" symptom: previously every
          // board reconnect killed Chrome and relaunched it (page reload +
          // screencast restart = blank flash on the 2 GB Pi5).
          // (Historically this block instead SHUT DOWN the stale client, which
          // is what caused the teardowns.)
          const donor = [...clients].find(cl => cl !== c && cl.browser && !cl._dead);
          if (donor) {
            let live = false;
            try {
              await Promise.race([
                donor.page.evaluate(() => 1),
                new Promise((_, rej) => setTimeout(() => rej(new Error('page probe timeout')), 8000)),
              ]);
              live = true;
            } catch (_) { live = false; }
            if (live) {
              c.adoptFrom(donor);
              clients.delete(donor);
              console.log(`[renderer] board (re)connected — adopted live session (no Chrome restart, client#${c._id} <- client#${donor._id}, framesSent=${c.framesSent})`);
              await c.resume(msg).catch(e => c.send({ type: 'err', msg: String(e && e.message || e) }));
            } else {
              console.log('[renderer] donor session dead — tearing it down, launching fresh');
              try { await donor.shutdown(); } catch (_) {}
              clients.delete(donor);
              await c.connect(msg);
            }
          } else {
            await c.connect(msg);
          }
          active = c;
          // Defensive: at most ONE live Chrome. Reap any other stale Chrome-
          // holding client (shouldn't happen now, but bounds the historical
          // 13-Chrome runaway on the 2 GB Pi5). The active session (c) is kept.
          for (const oc of [...clients]) {
            if (oc !== c && oc.browser && !oc._dead) {
              console.log('[renderer] reaping extra stale Chrome client');
              try { await oc.shutdown(); } catch (_) {}
              clients.delete(oc);
            }
          }
          break;
        }
        case 'navigate': await c.navigate(msg.url); break;
        case 'reload':   await c.reload(); break;
        case 'input':    await c.input(msg); break;
        case 'ping':
          c.send({ type: 'pong' });
          // DIAG: which physical client does the board ping arrive on, and does
          // the pong go to the SAME socket that ticks/frames use (active)? A
          // mismatch (pong to a zombie client, frames to active) would explain
          // pongs never reaching the board while ticks do.
          {
            const ip = c.ws._socket && c.ws._socket.remoteAddress || '?';
            console.log(`[renderer] PING on client#${c._id} ip=${ip} up=${Date.now() - c.connectedAt}ms -> PONG sent on client#${c._id} is_active=${active === c}`);
          }
          break;
        // `target: <ip>` lets a probe client drive ANOTHER client's page (the
        // board) instead of its own — needed to inspect/tap the board's live
        // Homey page without a second browser.
        function normIp(s) {
          if (!s) return '';
          s = String(s);
          // ::ffff:1.2.3.4 -> 1.2.3.4
          const m = s.match(/\.(\d+\.\d+\.\d+\.\d+)$/);
          return m ? m[1] : s;
        }
        function targetClient(msg) {
          if (!msg.target) return c;
          const want = normIp(msg.target);
          return [...clients].find((cl) => normIp(cl.ws._socket && cl.ws._socket.remoteAddress) === want) || c;
        }
        case 'tap': {
          // CDP tap at page coords — identical path to a board touch, which
          // lands in the same client's c.input() -> page.mouse.
          const t = targetClient(msg);
          const m = t.page.mouse;
          const x = msg.x | 0, y = msg.y | 0;
          await m.move(x, y); await m.down({ button: 'left' });
          await new Promise(r => setTimeout(r, 60));
          await m.up({ button: 'left' });
          c.send({ type: 'tapped', x, y, target: msg.target || 'self' });
          break;
        }
        case 'nav': {
          // Point the target client's (the board's) Chrome at msg.url — used to
          // load a known solid-color test page for the color-fidelity check.
          const t = targetClient(msg);
          await t.navigate(msg.url);
          c.send({ type: 'navd', target: msg.target || 'self' });
          break;
        }
        case 'dom': {
          // Find clickable toggle-ish elements + their state, in PAGE space.
          const t = targetClient(msg);
          const info = await t.page.evaluate(() => {
            const out = [];
            const sel = 'button, [role=switch], [role=checkbox], input[type=checkbox], [class*=switch], [class*=toggle], [class*=Switch]';
            for (const el of document.querySelectorAll(sel)) {
              const r = el.getBoundingClientRect();
              if (r.width < 12 || r.height < 12 || r.width > 400 || r.height > 400) continue;
              out.push({
                tag: el.tagName, role: el.getAttribute('role'),
                cls: (el.className || '').toString().slice(0, 60),
                aria: el.getAttribute('aria-label') || el.getAttribute('aria-pressed') || el.getAttribute('aria-checked') || '',
                cx: Math.round(r.x + r.width / 2), cy: Math.round(r.y + r.height / 2),
                w: Math.round(r.width), h: Math.round(r.height),
                text: (el.textContent || '').trim().slice(0, 40),
              });
            }
            return { url: location.href, title: document.title, n: out.length, items: out.slice(0, 40) };
          });
          c.send({ type: 'dom', ...info });
          break;
        }
        case 'dom2': {
          const t = targetClient(msg);
          const info = await t.page.evaluate(() => {
            const out = [];
            const sel = 'button, [role=switch], [role=checkbox], [role=button], [role=slider], [class*=switch], [class*=toggle], [class*=device], [class*=valve], [class*=light], [class*=shutter], [class*=plug], [class*=thermostat], [class*=widget]';
            const seen = new Set();
            for (const el of document.querySelectorAll(sel)) {
              const r = el.getBoundingClientRect();
              if (r.width < 14 || r.height < 14 || r.width > 420 || r.height > 420) continue;
              const key = el.tagName + '|' + Math.round(r.x) + ',' + Math.round(r.y) + '|' + (el.className || '').toString().slice(0, 40);
              if (seen.has(key)) continue; seen.add(key);
              out.push({
                tag: el.tagName, role: el.getAttribute('role'),
                cls: (el.className || '').toString().slice(0, 70),
                aria: (el.getAttribute('aria-label') || '').slice(0, 50),
                pressed: el.getAttribute('aria-pressed'),
                checked: el.getAttribute('aria-checked'),
                cx: Math.round(r.x + r.width / 2), cy: Math.round(r.y + r.height / 2),
                w: Math.round(r.width), h: Math.round(r.height),
                text: (el.textContent || '').trim().slice(0, 40),
              });
            }
            return { url: location.href, title: document.title, n: out.length, items: out.slice(0, 80) };
          });
          c.send({ type: 'dom2', ...info });
          break;
        }
        case 'theme': {
          const t = targetClient(msg);
          const s = await t.page.evaluate(() => {
            const b = document.querySelector('[aria-label^="Editor theme"]');
            return {
              themeLabel: b ? b.getAttribute('aria-label') : 'none',
              bg: getComputedStyle(document.body).backgroundColor,
            };
          });
          c.send({ type: 'theme', target: msg.target || 'self', ...s });
          break;
        }
        case 'textels': {
          const t = targetClient(msg);
          const info = await t.page.evaluate(() => {
            const out = [];
            const seen = new Set();
            // leaf-ish text elements
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT);
            while (walker.nextNode()) {
              const el = walker.currentNode;
              const txt = (el.childNodes.length && [...el.childNodes].some(n => n.nodeType === 3 && n.textContent.trim())
                ? (el.textContent || '').trim() : '');
              if (!txt || txt.length > 40) continue;
              const r = el.getBoundingClientRect();
              if (r.width < 8 || r.height < 8) continue;
              const key = Math.round(r.x) + ',' + Math.round(r.y) + '|' + txt.slice(0, 12);
              if (seen.has(key)) continue; seen.add(key);
              out.push({
                cx: Math.round(r.x + r.width / 2), cy: Math.round(r.y + r.height / 2),
                w: Math.round(r.width), h: Math.round(r.height),
                role: el.getAttribute('role'),
                pressed: el.getAttribute('aria-pressed'),
                checked: el.getAttribute('aria-checked'),
                aria: (el.getAttribute('aria-label') || '').slice(0, 40),
                text: txt.slice(0, 40),
              });
            }
            return { url: location.href, n: out.length, items: out.slice(0, 120) };
          });
          c.send({ type: 'textels', ...info });
          break;
        }
        case 'canvas': {
          const t = targetClient(msg);
          const info = await t.page.evaluate(() => {
            const out = [];
            const seen = new Set();
            const ifr = [];
            for (const f of document.querySelectorAll('iframe')) ifr.push(f.src || '(blank)');
            const els = document.querySelectorAll('div,button,[role],svg');
            for (const el of els) {
              const r = el.getBoundingClientRect();
              if (r.left < 280) continue;               // main canvas only
              if (r.width < 20 || r.height < 20 || r.width > 780 || r.height > 1260) continue;
              const dataAttrs = {};
              for (const a of el.attributes || []) if (a.name.startsWith('data-')) dataAttrs[a.name] = a.value;
              const hasDeviceId = JSON.stringify(dataAttrs).match(/[0-9a-f]{8}-[0-9a-f]{4}/i);
              const key = Math.round(r.x) + ',' + Math.round(r.y) + '|' + (el.className || '').toString().slice(0, 30);
              if (seen.has(key)) continue; seen.add(key);
              if (!hasDeviceId && !el.getAttribute('role') && !el.onclick && r.width < 40) continue;
              out.push({
                tag: el.tagName, role: el.getAttribute('role'),
                cx: Math.round(r.x + r.width / 2), cy: Math.round(r.y + r.height / 2),
                w: Math.round(r.width), h: Math.round(r.height),
                cls: (el.className || '').toString().slice(0, 60),
                data: JSON.stringify(dataAttrs).slice(0, 80),
                text: (el.textContent || '').trim().slice(0, 30),
              });
            }
            return { url: location.href, iframes: ifr, n: out.length, items: out.slice(0, 60) };
          });
          c.send({ type: 'canvas', ...info });
          break;
        }
        case 'diag': {
          const list = [...clients].map((cl) => ({
            ip: cl.ws._socket && cl.ws._socket.remoteAddress || '?',
            framesSent: cl.framesSent,
            browserUp: cl.browserUp,
            upMs: Date.now() - cl.connectedAt,
            viewport: cl.width + 'x' + cl.height,
          }));
          c.send({ type: 'diag', clients: list, count: clients.size });
          break;
        }
        case 'shot': {
          const t = targetClient(msg);
          const shot = await t.page.screenshot({
            type: 'jpeg', quality: 90,
            clip: { x: 0, y: 0, width: t.width, height: t.height },
          });
          c.send({ type: 'shot', target: msg.target || 'self',
            w: t.width, h: t.height, jpeg: Buffer.from(shot).toString('base64') });
          break;
        }
        case 'find': {
          // Locate elements by text substring (whole page, no 120-item cap) — used to
          // find a specific dashboard control (e.g. a Light Grid switch) by label.
          const t = targetClient(msg);
          const info = await t.page.evaluate((needle) => {
            const out = [];
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT);
            while (walker.nextNode()) {
              const el = walker.currentNode;
              const txt = (el.textContent || '').trim();
              if (txt.length < 2 || txt.length > 60 || !txt.includes(needle)) continue;
              const r = el.getBoundingClientRect();
              if (r.width < 8 || r.height < 8) continue;
              out.push({ cx: Math.round(r.x + r.width / 2), cy: Math.round(r.y + r.height / 2), w: Math.round(r.width), h: Math.round(r.height), text: txt.slice(0, 40), role: el.getAttribute('role'), pressed: el.getAttribute('aria-pressed'), checked: el.getAttribute('aria-checked'), cls: (el.className || '').toString().slice(0, 50) });
            }
            return out.slice(0, 30);
          }, msg.needle || '');
          c.send({ type: 'found', needle: msg.needle || '', n: info.length, items: info });
          break;
        }
        default: c.send({ type: 'err', msg: 'unknown type ' + msg.type });
      }
    } catch (e) {
      console.error('[renderer] msg handler error:', e && e.message || e);
      try { c.send({ type: 'err', msg: String(e && e.message || e) }); } catch (_) {}
    }
  });
  ws.on('close', async () => {
    console.log(`[renderer] client#${c._id} closed`);
    if (c === active && c.browser && !c._dead) {
      // The session owner's board WS dropped (flapping). Keep the Chrome +
      // page + screencast alive for SESSION_GRACE_MS in case the board comes
      // straight back (it reconnects within a few seconds) and adopts the
      // session — no Chrome restart, no blank. Only tear down if it doesn't.
      //
      // IMPORTANT: `c` stays in the `clients` registry (detached) so the
      // reconnecting board's `connect` handler can FIND it as an adoption
      // donor. It is removed either on adoption or when the grace teardown
      // fires below.
      //
      // NOTE: start the timer at most ONCE — the idle sweeper also calls
      // ws.close() on this (already-closing) socket every 15s and must not
      // keep re-arming the timer, or a dead board's Chrome would never be
      // released.
      if (!c._graceTimer) {
        c._graceTimer = setTimeout(async () => {
          c._graceTimer = null;
          if (active === c) {
            console.log('[renderer] board absent past grace — tearing down session');
            active = null;
            clients.delete(c);
            await c.shutdown();
          }
        }, SESSION_GRACE_MS);
      }
    } else {
      // Transient (probe) client, or a client that already handed its session
      // off via adoptFrom(): nothing to keep.
      clients.delete(c);
      await c.shutdown();
    }
  });
});

// Idle-timeout sweeper: a client that launched Chrome (browserUp) but goes
// silent (no ping/message) for IDLE_TIMEOUT is considered dead — WiFi drop or a
// board whose remote_task stalled — and its socket is closed so the session
// can be released (the close handler then applies the grace period before any
// Chrome teardown). A live board pings every 20s, so it never trips this.
// Only sockets we believe are OPEN are closed: a detached session owner (board
// gone, in grace) stays in the registry but must not be re-closed every 15s.
const IDLE_TIMEOUT_MS = 45000;
setInterval(() => {
  const now = Date.now();
  for (const c of [...clients]) {
    if (c.browserUp && c.ws && c.ws.readyState === 1 && (now - c.lastMsgAt) > IDLE_TIMEOUT_MS) {
      console.log('[renderer] client idle >' + (IDLE_TIMEOUT_MS / 1000) + 's (has Chrome) — closing to release session');
      try { c.ws.close(); } catch (_) {}
    } else if (!c.browserUp && c !== active && c.ws && c.ws.readyState === 1 &&
               (now - c.lastMsgAt) > 45000) {
      // Zombie: a board socket whose FIN never arrived (the board's lwIP
      // defers the FIN while unread RX data sits in the abandoned buffer).
      // It never gets messages, so lastMsgAt is frozen at connect time;
      // reaping from our side lets the close handler drop it from the
      // registry and clear its tick interval. 45s >> the board's 20s ping
      // cadence, so a live client is never caught by this.
      console.log(`[renderer] reaping zombie client#${c._id} (no Chrome, silent ${((now - c.lastMsgAt) / 1000) | 0}s)`);
      try { c.ws.close(); } catch (_) {}
    }
  }
}, 15000);

// SIGTERM/SIGINT: shut down every Client (close its Chrome). NOTE: iterate the
// `clients` Set of Client objects — `wss.clients` is the WebSocketServer's set
// of raw WebSocket connections, which have no .shutdown(), and iterating it
// here used to crash the handler (TypeError: c.shutdown is not a function)
// before process.exit() could run.
process.on('SIGTERM', async () => { for (const c of [...clients]) { try { await c.shutdown(); } catch (_) {} } process.exit(0); });

// On-demand control over plain HTTP — refresh the board's page with one curl,
// no board reboot, no WS client needed. A Homey flow (or me) hits /reload
// after a dashboard edit; the renderer re-fetches the pinned page and the
// fresh frames flow to the board.
//   curl http://<nas>:5052/reload          -> re-fetch the pinned page
//   curl 'http://<nas>:5052/nav?u=URL'     -> point the board's page at URL
//   curl http://<nas>:5052/status          -> what it's rendering + frame count
const CONTROL_PORT = parseInt(process.env.RENDERER_CONTROL_PORT, 10) || 5052;
try {
  const cthttp = require('http');
  cthttp.createServer((req, res) => {
    let u; try { u = new URL(req.url, 'http://localhost'); } catch (_) { res.statusCode = 400; return res.end('bad url'); }
    res.setHeader('Content-Type', 'application/json');
    const finish = (code, obj) => { res.statusCode = code; res.end(JSON.stringify(obj)); };
    const a = active;
    if (!a || !a.page) return finish(503, { ok: false, msg: 'no active page (board not connected)' });
    if (u.pathname === '/reload') {
      a.reload().then(() => finish(200, { ok: true, url: a.page.url() }))
                .catch(e => finish(500, { ok: false, msg: String(e && e.message || e) }));
    } else if (u.pathname === '/nav') {
      const target = u.searchParams.get('u');
      if (!target) return finish(400, { ok: false, msg: 'usage: /nav?u=<url>' });
      a.navigate(target).then(() => finish(200, { ok: true, url: a.page.url() }))
                        .catch(e => finish(500, { ok: false, msg: String(e && e.message || e) }));
    } else if (u.pathname === '/status') {
      return finish(200, { ok: true, url: a.page.url(),
        framesSent: a.framesSent || 0, clients: clients.size, pinUrl: PIN_URL || null });
    } else {
      return finish(404, { ok: false, msg: 'endpoints: /reload, /nav?u=<url>, /status' });
    }
  }).listen(CONTROL_PORT, '0.0.0.0', () =>
    console.log(`[renderer] control HTTP on :${CONTROL_PORT} (/reload /nav /status)`));
} catch (e) {
  console.log(`[renderer] control HTTP failed to start: ${e && e.message}`);
}
process.on('SIGINT',  async () => { for (const c of [...clients]) { try { await c.shutdown(); } catch (_) {} } process.exit(0); });
