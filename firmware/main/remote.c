/*
 * remote.c — Option B: NAS-rendered Homey dashboard on the P4 display.
 *
 * Connects a hand-rolled WebSocket client to the NAS renderer (renderer.js,
 * ws://NAS_IP:9100). The renderer runs full Chrome, loads the Homey SPA and
 * streams changed JPEG frames. We request a PORTRAIT 800x1280 viewport
 * (matching the panel) so frames arrive 1:1 — no rotation, no scaling — and
 * touch coordinates map 1:1 to page coordinates (renderer's rotateToPage is
 * false for a portrait board + portrait viewport).
 *
 * Pipeline:
 *   TCP -> WS handshake -> JSON text frames
 *     {type:frame, jpeg:base64} -> base64 decode -> HW JPEG decoder (RGB565)
 *       written STRAIGHT into the panel framebuffer s_fb0 via the driver's
 *       DMA2D (2MB, 4B aligned; the driver cache-invalidates it).
 *   GSL3680 poll (10ms) -> {type:input, kind:down/move/up, x, y} upstream.
 *
 * While remote UI is active, browser.c's lvgl_flush_cb and touch_read_cb are
 * gated (see remote_ui_active()) so LVGL stops overwriting s_fb0 and stops
 * double-reading the touch I2C bus — the video owns the panel and this task
 * is the sole touch reader.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_netif.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_gsl3680.h"
#include "driver/jpeg_decode.h"
#include "nvs.h"
#include "settings.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *RTAG = "remote";

/* Renderer endpoint — NVS-backed ("remote" namespace: host/port/url), editable
 * on-board via the settings screen (3 s long-press). Defaults: the Synology
 * NAS at 192.168.1.100 (portable headless Chrome, reached from the board's
 * subnet via the .3 gateway — the NAS's .3.100 address sits on a different L2
 * segment the board cannot ARP (2026-11-01). Moved off the Pi5 192.168.3.11
 * the same day: the Pi5 thermally/PSU-throttled and stalled the frame stream,
 * tripping the 30 s watchdog.) The dashboard URL is loaded BY the renderer on
 * the NAS, so 127.0.0.1 is correct from its point of view. (Historically a
 * #define pointed at 192.168.3.5:5050 and only worked because the renderer's
 * PIN_URL env var overrode it — the board is the source of truth now.) */
static char s_cfg_host[64] = CFG_DEFAULT_HOST;
static int  s_cfg_port     = CFG_DEFAULT_PORT;
static char s_cfg_url[160] = CFG_DEFAULT_URL;

/* Load NVS overrides (no-op if never saved). Called at the top of every
 * remote_task run, so a settings-screen save + restart picks up new values. */
static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("remote", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(s_cfg_host) - 1;
    if (nvs_get_str(h, "host", s_cfg_host, &l) != ESP_OK)
        strlcpy(s_cfg_host, CFG_DEFAULT_HOST, sizeof(s_cfg_host));
    uint16_t p = 0;
    if (nvs_get_u16(h, "port", &p) != ESP_OK || p == 0) p = CFG_DEFAULT_PORT;
    s_cfg_port = p;
    l = sizeof(s_cfg_url) - 1;
    if (nvs_get_str(h, "url", s_cfg_url, &l) != ESP_OK)
        strlcpy(s_cfg_url, CFG_DEFAULT_URL, sizeof(s_cfg_url));
    nvs_close(h);
}

#define PANEL_W        800
#define PANEL_H        1280
/* The renderer's landscape PAGE/viewport. The JC8012P4A1 panel is
 * native-portrait (800x1280) but mounted landscape, so the page is 1280x800
 * and the board rotates each decoded frame 90deg into the portrait framebuffer. */
#define PAGE_W         1280
#define PAGE_H         800
/* Rotation orientation tuning. native(nx,ny) shows page(px,py) with
 *   px = ROT_FLIP_X ? ny : (PAGE_W-1-ny)
 *   py = ROT_FLIP_Y ? (PAGE_H-1-nx) : nx
 * Default (0,0) derived from a camera shot of the mounted panel. If content
 * comes out upside-down/mirrored, toggle these and rebuild. */
#define ROT_FLIP_X     0
#define ROT_FLIP_Y     0
#define FB_BYTES       (PANEL_W * PANEL_H * 2)   /* 2,048,000 — panel RGB565 */
#define RGB888_BYTES   (PANEL_W * PANEL_H * 3)   /* 3,072,000 — temp decode RGB888 (full 8-bit ch) */

/* ---- wiring into browser.c (it owns the touch driver + the framebuffer) ---- */
extern esp_lcd_touch_handle_t remote_touch_handle(void);   /* returns s_touch */
extern void *remote_fb_addr(void);                          /* returns s_fb0 (uint8_t*) */
extern void   remote_ui_ready(void);                        /* hide text UI (browser.c) */
extern void   browser_indev_swallow_release(void);          /* (browser.c) drop the gesture-hold's release */

/* ---- stats (reported in /state) ---- */
static volatile int     s_connect_attempts = 0;
static volatile int     s_frames_rx        = 0;
static volatile int     s_frames_decoded   = 0;
static volatile int     s_frames_failed    = 0;
static volatile int     s_input_sent       = 0;
static volatile int     s_touch_pts        = 0;    /* touch_poll calls where point_num>0 (gsl3680 actually reporting a touch) */
static volatile int     s_touch_tpvalid    = -1;   /* 1 if remote_touch_handle() non-NULL in last poll, 0 if NULL; -1 = never polled */
static volatile int     s_touch_lastpts    = -1;   /* last point_num seen by touch_poll; -1 = never polled */
static volatile int     s_reconnects       = 0;
/* remote_task xTaskCreate result + task count at creation, surfaced in /state
 * (the serial log of these is lost to UART overrun at boot). */
static volatile int     s_remote_task_rc   = -99;
static volatile int     s_remote_task_n    = -99;
static volatile int     s_remote_heap_kb   = -99;
static volatile int     s_heap_int_kb      = -99;   /* internal RAM free at remote_start entry */
static volatile int     s_heap_psram_kb    = -99;   /* PSRAM free at remote_start entry */
static volatile int     s_sock             = -1;
/* Guards the WS socket for concurrent SEND: the remote_task (connect + ping)
 * and the dedicated touch task both call ws_send_text on the same socket.
 * Without this, two masked client frames can interleave on the wire and the
 * renderer's WS parser desyncs (frames dropped). Receive stays remote_task-only. */
static SemaphoreHandle_t s_ws_send_lock    = NULL;
static volatile bool    s_remote_active    = false;   /* WS connected right now */
static volatile int64_t s_last_frame_ms    = 0;
static volatile int64_t s_last_msg_ms      = 0;   /* last inbound WS msg of ANY type (frames, hello, tick) — used to detect a half-open socket (board can send pings but renderer->board direction is dead) and force a reconnect */
static volatile int64_t s_last_pong_ms     = 0;   /* last 'pong' from the renderer. The board pings every 20s and the renderer replies 'pong'. If frames keep arriving (renderer->board alive) but the pong stops, the BOARD->renderer direction is dead (our pings/touch are being dropped). recv never errors and s_last_msg_ms stays fresh from inbound frames, so neither the silent- nor the no-frame check fires — the pong timeout is the ONLY way to catch this one-directional half-open state. */
/* Boot watchdog: if no frame has EVER been received by the time we'd expect
 * one, the renderer (or Wi-Fi) is down — the black gated screen the user then
 * sees is a dead end. Auto-open the config screen with instructions instead,
 * and close it the moment video recovers. s_boot_hint_open = WE opened it. */
static volatile bool    s_boot_hint_open   = false;
static volatile int64_t s_boot_ref_ms      = 0;    /* remote_task connect-loop start */
static char s_last_err[96] = {0};
static char s_title[96]    = {0};

static uint8_t *s_fb0       = NULL;   /* panel frame buffer (RGB565) — the video path reads this */
static uint8_t *s_fb1       = NULL;   /* temp decode buffer (RGB888, 3B/px): decode here, convert, blit to s_fb0 */
static volatile int s_color_fix = 1;  /* 1 = apply channel conversion after JPEG decode (fixes DMA2D CSC 3-cycle) */
static volatile uint8_t s_raw888_center[3] = {0,0,0}; /* raw decoder RGB888 center px — for channel-order diagnosis */
static uint8_t *s_jpeg_in   = NULL;   /* base64-decoded JPEG (PSRAM) */
static uint8_t *s_rx        = NULL;   /* raw WS frame (PSRAM) */
static size_t   s_jpeg_in_sz = 0;     /* capacity of s_jpeg_in (pointer: sizeof() is useless) */
static size_t   s_rx_sz      = 0;     /* capacity of s_rx (pointer: sizeof() is useless) */

/* ---- present pipeline (v6) ----
 * The WS receive loop used to call present_frame() inline: a full HW-JPEG
 * decode + RGB888->565 convert + DSI blit (~250 ms/frame) blocked the socket
 * drain, so pongs/ticks queued behind frames, pong latency tripped the 40 s
 * npong watchdog, and the effective rate collapsed to ~1 fps (the renderer
 * sends ~8). Now a dedicated present task owns the decode: the receiver
 * hands a frame to it via a single-slot staging buffer (s_jpeg_in) and keeps
 * draining the socket at line rate; a frame that arrives while the presenter
 * is still busy is DROPPED — the renderer re-sends within ~125 ms, so one
 * dropped frame costs nothing visible.
 * s_buf_free : counting(1,1) — token held while the PRESENTER owns s_jpeg_in.
 * s_frame_evt: counting(1,0) — a staged JPEG awaits presentation. */
static SemaphoreHandle_t s_buf_free  = NULL;
static SemaphoreHandle_t s_frame_evt = NULL;
static volatile size_t   s_frame_len = 0;
static volatile bool     s_present_task_alive = false;

/* ============================ PRNG (WS mask) ============================ */
static uint32_t s_lcg = 0x12345678u;
static uint32_t lcg_random(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

/* ============================ base64 ============================ */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const uint8_t *src, size_t slen, uint8_t *dst, size_t dcap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < slen && o < dcap; i++) {
        int v = b64_val((char)src[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            dst[o++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }
    return o;
}

/* Locate "jpeg":"<b64>" and decode it. Returns decoded byte length (0=none). */
static size_t extract_jpeg(const char *json, uint8_t *dst, size_t dcap)
{
    const char *p = strstr(json, "\"jpeg\":\"");
    if (!p) return 0;
    p += 8;
    const char *e = strchr(p, '"');
    if (!e) return 0;
    return b64_decode((const uint8_t *)p, (size_t)(e - p), dst, dcap);
}

/* ============================ path probe (diag) ============================ */

static void probe_to(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char p[8];
    snprintf(p, sizeof(p), "%d", port);
    if (getaddrinfo(host, p, &hints, &res) != 0 || !res) {
        ESP_LOGW(RTAG, "probe %s:%d dns-fail", host, port);
        return;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return; }
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r == 0) {
        ESP_LOGI(RTAG, "probe %s:%d -> OK", host, port);
        close(sock);
    } else {
        int e = errno;
        ESP_LOGW(RTAG, "probe %s:%d -> FAIL errno=%d (%s)", host, port, e, strerror(e));
        close(sock);
    }
}

static void probe_path(void)
{
    ESP_LOGI(RTAG, "=== PATH PROBE ===");
    /* ICMP echo via raw socket: isolates ARP/route failure (which makes every
     * TCP probe report EHOSTUNREACH) from a pure firewall drop. */
    {
        static int s_ping_rx;
        u8_t ping_rx_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
        {
            (void)arg; (void)pcb; (void)addr;
            if (p) s_ping_rx++;
            return 1; /* 1 = pbuf freed */
        }
        s_ping_rx = 0;
        struct raw_pcb *raw = raw_new(IP_PROTO_ICMP);
        if (!raw) {
            ESP_LOGW(RTAG, "ping: raw_new failed");
        } else {
            raw_recv(raw, ping_rx_cb, NULL);
            ip_addr_t gw = IPADDR4_INIT_BYTES(192, 168, 3, 1);
            for (int i = 0; i < 2; i++) {
                struct pbuf *p = pbuf_alloc(PBUF_RAW, 16, PBUF_RAM);
                if (p) {
                    uint8_t pkt[16] = {8, 0, 0xf6, 0xf3, 1, 2, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4};
                    pbuf_take(p, pkt, 16); /* fixed payload, chksum 0xf6f3 precomputed */
                    err_t se = raw_sendto(raw, p, &gw);
                    if (se != ERR_OK) ESP_LOGW(RTAG, "ping send %d: %d", i, (int)se);
                    else pbuf_free(p);
                }
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            int rec = s_ping_rx;
            raw_remove(raw);
            ESP_LOGI(RTAG, "ping 192.168.3.1: rec=%d (2 = LAN path alive)", rec);
        }
    }
    probe_to("192.168.3.1", 80, 5000);      /* .3 gateway */
    probe_to(s_cfg_host, s_cfg_port, 5000); /* renderer endpoint (NVS config) */
    ESP_LOGI(RTAG, "=== PATH PROBE DONE ===");
}

/* ============================ WebSocket ============================ */

static int ws_send_all(int sock, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(sock, (const char *)buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Send a masked (client->server) text frame. */
static int ws_send_text(int sock, const char *payload, size_t len)
{
    uint8_t hdr[14];
    size_t hl = 0;
    hdr[hl++] = 0x81;                    /* FIN + text */
    if (len < 126) {
        hdr[hl++] = (uint8_t)len | 0x80; /* MASK bit */
    } else if (len < 65536) {
        hdr[hl++] = 126 | 0x80;
        hdr[hl++] = (uint8_t)(len >> 8);
        hdr[hl++] = (uint8_t)(len & 0xFF);
    } else {
        return -1;                       /* upstream frames are tiny */
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) { mask[i] = (uint8_t)lcg_random(); hdr[hl++] = mask[i]; }
    if (ws_send_all(sock, hdr, hl) != 0) return -1;
    uint8_t *m = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!m) return -1;
    for (size_t i = 0; i < len; i++) m[i] = (uint8_t)payload[i] ^ mask[i & 3];
    int r = ws_send_all(sock, m, len);
    heap_caps_free(m);
    return r;
}

/* Mutexed send: safe for the remote_task (ping/connect) and the touch task to
 * call on the same socket concurrently. No-op (returns -1) if the lock wasn't
 * created or the socket is closed. */
static int ws_send_locked(const char *payload, size_t len)
{
    if (!s_ws_send_lock || s_sock < 0) return -1;
    xSemaphoreTake(s_ws_send_lock, pdMS_TO_TICKS(100));
    int r = ws_send_text(s_sock, payload, len);
    xSemaphoreGive(s_ws_send_lock);
    return r;
}

static int rd_all(int sock, uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int k = recv(sock, (char *)buf + off, n - off, 0);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; /* RCVTIMEO */
            return -1;
        }
        if (k == 0) return -1;
        off += (size_t)k;
    }
    return 0;
}

/* Read one WS frame. Returns 1 on data frame (*out_len), 0 on control
 * consumed, -1 on close/error, -2 on recv timeout (keep the connection). */
static int ws_read_frame(int sock, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t h[2];
    int r = rd_all(sock, h, 2);
    if (r != 0) return r;
    int op     = h[0] & 0x0F;
    int masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t x[2];
        if ((r = rd_all(sock, x, 2)) != 0) return r;
        len = ((uint64_t)x[0] << 8) | x[1];
    } else if (len == 127) {
        uint8_t x[8];
        if ((r = rd_all(sock, x, 8)) != 0) return r;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | x[i];
    }
    uint8_t mask[4] = {0};
    if (masked) { if ((r = rd_all(sock, mask, 4)) != 0) return r; }
    if (len > cap) {
        /* drain oversized frame then drop */
        uint8_t tmp[1024];
        uint64_t left = len;
        while (left) {
            size_t c = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
            if (rd_all(sock, tmp, c) != 0) return -1;
            left -= c;
        }
        return 0;
    }
    /* Timeout mid-frame (after consuming the header) desyncs the stream —
     * the only recoverable timeout is at the 2-byte header read (returns -2).
     * Anything later here forces a reconnect. */
    if (rd_all(sock, buf, (size_t)len) != 0) return -1;
    if (masked) { for (size_t i = 0; i < len; i++) buf[i] ^= mask[i & 3]; }

    switch (op) {
    case 0x0: case 0x1: case 0x2:   /* continuation / text / binary */
        *out_len = (size_t)len;
        return 1;
    case 0x8:                        /* close */
        ESP_LOGI(RTAG, "ws closed by server");
        return -1;
    case 0x9:                        /* ping -> pong (masked, empty) */
    {
        uint8_t ph[6] = {0x8A, 0x80, 0, 0, 0, 0};
        for (int i = 0; i < 4; i++) ph[2 + i] = (uint8_t)lcg_random();
        ws_send_all(sock, ph, 6);
        return 0;
    }
    case 0xA:                        /* pong */
    default:
        return 0;
    }
}

static int ws_connect(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", s_cfg_port);
    if (getaddrinfo(s_cfg_host, port, &hints, &res) != 0 || !res) {
        snprintf(s_last_err, sizeof(s_last_err), "dns");
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); snprintf(s_last_err, sizeof(s_last_err), "socket"); return -1; }
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int cr = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr != 0) {
        close(sock);
        snprintf(s_last_err, sizeof(s_last_err), "connect");
        return -1;
    }
    char hs[256];
    int hl = snprintf(hs, sizeof(hs),
        "GET / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        s_cfg_host, s_cfg_port);
    if (ws_send_all(sock, (uint8_t *)hs, (size_t)hl) != 0) {
        close(sock); snprintf(s_last_err, sizeof(s_last_err), "ws-hs-send"); return -1;
    }
    uint8_t rb[512];
    size_t rl = 0;
    for (;;) {
        int n = recv(sock, (char *)rb + rl, sizeof(rb) - 1 - rl, 0);
        if (n <= 0) { close(sock); snprintf(s_last_err, sizeof(s_last_err), "ws-hs-rd"); return -1; }
        rl += (size_t)n;
        rb[rl] = 0;
        if (strstr((char *)rb, "\r\n\r\n")) break;
        if (rl > sizeof(rb) - 1) { close(sock); snprintf(s_last_err, sizeof(s_last_err), "ws-hs-big"); return -1; }
    }
    if (!strstr((char *)rb, "101")) {
        close(sock);
        snprintf(s_last_err, sizeof(s_last_err), "ws-hs-rej");
        return -1;
    }
    /* Data phase: the renderer only pushes frames on change, so idle gaps can
     * exceed the handshake timeout. 10s (was 30s): the remote_task now loops
     * every 10s, so the silence detector notices a half-open socket within
     * ~30s and the ping cadence stays at its intended 20s. */
    struct timeval tv2 = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
    /* TCP keepalive — transport-layer safety net for a silently-dead
     * connection (whole socket dead, neither side notices). The application
     * pong timeout in remote_task catches the one-directional case (frames
     * still flowing but our sends dropped); keepalive catches the both-ways
     * case. Probe after 20s idle, every 5s, 3 failures => recv errors. */
    int ka = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    int ka_idle = 20, ka_intvl = 5, ka_cnt = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &ka_cnt, sizeof(ka_cnt));
    s_sock = sock;
    return 0;
}

/* ============================ frame pipeline ============================ */

/* The HW JPEG decoder's DMA2D CSC 3-cycles channels in its RGB565 output (true
 * green lands in a 5-bit slot → dimmed to 44%). To get correct color + full
 * 6-bit green we decode to RGB888 (each channel full 8-bit) and convert to the
 * panel's RGB565 ourselves. s_rgb888_order selects the decoder's byte order
 * (determined from the solid-color diagnosis): 0 = (R,G,B); 1 = (B,R,G). */
static volatile int s_rgb888_order = 0;
/* Build 98: cache-friendly 90deg rotation (tile transpose).
 * Build 97's per-pixel strided loop read PSRAM one pixel at a time — each
 * access fetched a 64B cache line but used 3B, so ~64MB of PSRAM traffic per
 * frame (~300-800ms). That blocks the single remote_task's ws_read_frame, so
 * the socket buffer overflows and the connection flaps into the freeze.
 * This tiles the frame into 32x32 blocks: read a source tile contiguously
 * (into a cache-resident SRAM buffer), transpose+convert in cache, then write
 * the dest tile contiguously. Total PSRAM traffic ~5MB/frame (~25-62ms), so
 * the board keeps up with the 12fps renderer and the WS receive never stalls.
 * Verified orientation (camera-confirmed upright): native(nx,ny) = page(px=PAGE_W-1-ny, py=nx).
 * PAGE_W=1280 and PAGE_H=800 are both exact multiples of ROT_TILE=32 (40 and 25
 * tiles), so no partial edge tiles are needed. */
#define ROT_TILE 32
static uint8_t  s_src_tile[ROT_TILE * ROT_TILE * 3] __attribute__((aligned(64)));
static uint16_t s_dst_tile[ROT_TILE * ROT_TILE]     __attribute__((aligned(64)));
static volatile int s_rot_ms = 0;   /* last rotation duration, for /state */

static void convert_rgb888_to_565(const uint8_t *src, uint16_t *dst, size_t n)
{
    (void)n;
    int64_t t0 = esp_timer_get_time();
    for (int c0 = 0; c0 < PAGE_W; c0 += ROT_TILE) {
        int ny0 = PAGE_W - c0 - ROT_TILE;          /* dest (native) tile row base */
        for (int r0 = 0; r0 < PAGE_H; r0 += ROT_TILE) {
            int nx0 = r0;                          /* dest (native) tile col base */
            /* 1) read source tile (rows r0..+T, cols c0..+T) contiguously */
            for (int r = 0; r < ROT_TILE; r++) {
                memcpy(s_src_tile + (size_t)r * ROT_TILE * 3,
                       src + ((size_t)(r0 + r) * PAGE_W + c0) * 3,
                       (size_t)ROT_TILE * 3);
            }
            /* 2) transpose + flip 90deg, convert RGB888->565, into s_dst_tile */
            for (int dy = 0; dy < ROT_TILE; dy++) {
                for (int dx = 0; dx < ROT_TILE; dx++) {
                    const uint8_t *s = &s_src_tile[(size_t)(dx * ROT_TILE + (ROT_TILE - 1 - dy)) * 3];
                    uint16_t v;
                    if (s_rgb888_order == 0) {
                        uint8_t R = s[0], G = s[1], B = s[2];
                        v = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
                    } else {
                        uint8_t b = s[0], r = s[1], g = s[2];
                        v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    }
                    s_dst_tile[dy * ROT_TILE + dx] = v;
                }
            }
            /* 3) write dest tile to dst (contiguous rows) */
            for (int dy = 0; dy < ROT_TILE; dy++) {
                memcpy(dst + (size_t)(ny0 + dy) * PANEL_W + nx0,
                       s_dst_tile + (size_t)dy * ROT_TILE,
                       (size_t)ROT_TILE * sizeof(uint16_t));
            }
        }
    }
    s_rot_ms = (int)((esp_timer_get_time() - t0) / 1000);
}

static void present_frame(const uint8_t *jpeg, size_t jlen)
{
    if (!s_fb0 || !s_fb1 || jlen == 0) return;   /* need the RGB888 temp buffer */
    static jpeg_decoder_handle_t dec = NULL;
    if (!dec) {
        jpeg_decode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 800 };
        if (jpeg_new_decoder_engine(&ecfg, &dec) != ESP_OK) {
            s_frames_failed++;
            snprintf(s_last_err, sizeof(s_last_err), "jpeg-engine");
            return;
        }
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    /* HW decode to RGB888 (full 8-bit channels) into the temp buffer, then CPU-
     * convert to the panel's RGB565 with correct 6-bit green. The DMA2D writes
     * PSRAM directly and cache-invalidates, so the CPU conversion reads fresh. */
    uint32_t out_size = 0;
    esp_err_t r = jpeg_decoder_process(dec, &cfg, jpeg, (uint32_t)jlen,
                                       s_fb1, (uint32_t)RGB888_BYTES, &out_size);
    if (r != ESP_OK) {
        s_frames_failed++;
        snprintf(s_last_err, sizeof(s_last_err), "decode 0x%x", (unsigned)r);
        return;
    }
    s_frames_decoded++;
    s_last_frame_ms = esp_timer_get_time() / 1000;

    if (s_color_fix && s_fb1) {
        int64_t t0 = esp_timer_get_time();
        /* dump raw decoder RGB888 center px (of the landscape page) for diagnosis */
        size_t ci = (size_t)(PAGE_H/2)*PAGE_W + (PAGE_W/2);
        s_raw888_center[0] = s_fb1[ci*3+0];
        s_raw888_center[1] = s_fb1[ci*3+1];
        s_raw888_center[2] = s_fb1[ci*3+2];
        /* convert RGB888 (s_fb1) -> RGB565 (s_fb0), correct 6-bit green */
        convert_rgb888_to_565(s_fb1, (uint16_t *)s_fb0, (size_t)PANEL_W*PANEL_H);
        esp_cache_msync((uint8_t *)s_fb0, (uint32_t)FB_BYTES, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if ((s_frames_decoded & 31) == 0)
            ESP_LOGI(RTAG, "conv888 %u px in %lld us (decoded=%u)", (unsigned)(PANEL_W*PANEL_H),
                     (long long)(esp_timer_get_time() - t0), (unsigned)s_frames_decoded);
    }
    if ((s_frames_decoded & 15) == 1) {
        ESP_LOGI(RTAG, "frame #%d (%u B out, failed=%d)", s_frames_decoded, out_size, s_frames_failed);
    }
}

/* ============================ touch -> upstream ============================ */

static int s_touch_down = 0;
static uint16_t s_tx = 0, s_ty = 0;

/* 3-second long-press (no movement) opens the settings screen — the board
 * has no physical config button. While settings is up, LVGL owns the touch
 * (browser.c indev falls through) and we suppress all upstream events. */
#define GESTURE_HOLD_MS   3000
#define GESTURE_MOVE_TOL  30
static int64_t s_press_t0 = 0;
static uint16_t s_press_x = 0, s_press_y = 0;
static int s_press_moved   = 0;
static int s_gesture_cfg   = 0;  /* long-press fired: swallow the "up" */
static int s_swallow_release = 0; /* swallow touches until the next clean release */
/* Long-press telemetry (build 76): per-press durations + a 16 s ring of RAW
 * chip finger presence (1 bit / 50 ms = 320 bits), hex in /state. After the
 * user does a hold at ANY time, /state shows exactly when the chip lost the
 * finger — no live coordination needed. */
static uint32_t s_press_n = 0;
static uint32_t s_press_last_ms = 0;
static uint32_t s_press_max_ms = 0;
static uint32_t s_gesture_fires = 0;
#define FHIST_BITS 320
static uint8_t s_fhist[FHIST_BITS / 8] = {0};
static uint32_t s_fhist_head = 0;
static volatile bool s_remote_stop = false;       /* remote_stop() requested */
static volatile bool s_remote_task_alive = false; /* remote_task running */
static volatile bool s_touch_task_alive  = false; /* touch_task running */

/* Present task: decodes + blits staged frames OFF the socket-drain path.
 * Prio 5 on core 1 — below remote_task (6), so the receiver always preempts
 * it and the socket is never queued behind a decode. Exits on s_remote_stop
 * (noticed at each 500 ms evt-timeout), within ~1.5 s of remote_stop(). */
static void present_task(void *arg)
{
    (void)arg;
    s_present_task_alive = true;
    ESP_LOGI(RTAG, "present task up");
    for (;;) {
        if (s_remote_stop) break;
        if (xSemaphoreTake(s_frame_evt, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (s_remote_stop) break;
        if (settings_active()) {   /* LVGL owns the fb — drop the frame */
            xSemaphoreGive(s_buf_free);
            continue;
        }
        present_frame(s_jpeg_in, s_frame_len);
        xSemaphoreGive(s_buf_free);   /* staging buffer free for the receiver */
    }
    s_present_task_alive = false;
    ESP_LOGI(RTAG, "present task down");
    vTaskDelete(NULL);
}

/* Remap a native-panel touch point (nx, ny) to the landscape PAGE coords the
 * renderer's page.mouse expects. Exact inverse of the frame rotation in
 * convert_rgb888_to_565, so a tap lands on the page pixel it is displayed at.
 * (The board reports itself as landscape, so the renderer passes these through.) */
static void touch_to_page(uint32_t nx, uint32_t ny, uint32_t *px, uint32_t *py)
{
    if (ny >= PAGE_W) ny = PAGE_W - 1;
    if (nx >= PAGE_H) nx = PAGE_H - 1;
    *px = ROT_FLIP_X ? ny : (PAGE_W - 1 - ny);
    *py = ROT_FLIP_Y ? (PAGE_H - 1 - nx) : nx;
}

/* 3 s long-press → open settings: run the LVGL screen build (keyboard!) in
 * its own 12 KB task — the 4 KB touch stack can't hold an LVGL build. */
static void cfg_open_worker(void *arg)
{
    (void)arg;
    /* The >3 s hold that fired this is still pressed. Swallow its release so
     * LVGL (now the sole touch reader once settings is up) can't turn it into
     * a click on a button under the finger — that was closing the screen the
     * instant it opened. Only the gesture path does this; HTTP open has no
     * pending finger. */
    browser_indev_swallow_release();
    settings_open();
    vTaskDelete(NULL);
}

/* Boot watchdog. remote_ui_ready() gates the display black and remote_task then
 * retries ws_connect() forever — so if the renderer (or Wi-Fi) is down after a
 * NAS reboot, the user is stuck on a black screen with no way to fix it. This
 * task gives them the config screen instead: if NO frame has arrived within a
 * grace window, open settings with "why you're seeing this" instructions. Once a
 * frame lands (renderer recovered), close it and retire. It only ever closes the
 * screen IT opened — a manual long-press is never touched. s_frames_rx (not
 * decoded) is the "stream alive" signal, because rx still counts frames while
 * the settings screen is up and decode is gated off. */
static void boot_watchdog(void *arg)
{
    (void)arg;
    const int64_t BOOT_GRACE_MS = 30000;   /* DHCP + C6 boot + Chrome cold start slack */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_remote_stop) break;
        if (s_frames_rx > 0) {              /* stream proved alive (even while gated) → done */
            if (s_boot_hint_open) { settings_close(); s_boot_hint_open = false; }
            break;
        }
        int64_t since = esp_timer_get_time() / 1000 - s_boot_ref_ms;
        if (since < BOOT_GRACE_MS) continue;
        if (s_boot_hint_open) {
            /* already showing it; if the user backed out of it, respect that */
            if (!settings_active()) break;
            continue;                        /* still up, waiting for the renderer to return */
        }
        if (settings_active()) continue;     /* user opened it themselves — don't fight them */
        ESP_LOGW(RTAG, "boot watchdog: no frame after %lld ms — opening config screen",
                 (long long)since);
        settings_open_boot();
        s_boot_hint_open = true;
    }
    ESP_LOGI(RTAG, "boot watchdog retiring (frames_rx=%d)", s_frames_rx);
    vTaskDelete(NULL);
}

static void touch_poll(void)
{
    /* Settings screen up: LVGL's indev is the sole touch reader (the GSL3680
     * I2C read path is not mutex-serialized — two readers wedge the bus). */
    if (settings_active()) {
        s_gesture_cfg = 0;
        s_touch_down = 0;
        return;
    }
    esp_lcd_touch_handle_t tp = remote_touch_handle();
    s_touch_tpvalid = (tp != NULL) ? 1 : 0;
    if (!tp || s_sock < 0) return;
    uint16_t x[5] = {0}, y[5] = {0}, str[5] = {0};
    esp_lcd_touch_read_data(tp);
    uint8_t point_num = 0;
    // get_coordinates returns true when touched and fills point_num. Track the
    // live position INSIDE the touched branch so the "up" event lands on the
    // real release point. (The old code set s_tx/s_ty only in the NOT-touched
    // branch, so every "up" went to (0,0) and a tap registered as a drag to the
    // top-left — the dashboard never saw it as a tap.)
    esp_lcd_touch_get_coordinates(tp, x, y, str, &point_num, 1);
    s_touch_lastpts = point_num;
    /* 16 s RAW finger-presence ring (50 ms bits) for /state — records the
     * chip's own count even while the vendor pipeline (or a swallow) hides
     * the finger from the host. */
    {
        static uint32_t fh_ct = 0;
        if (++fh_ct >= 4) {
            fh_ct = 0;
            uint32_t bit = s_fhist_head;
            s_fhist_head = (s_fhist_head + 1) % FHIST_BITS;
            if (esp_lcd_gsl3680_diag_raw_finger_num() > 0)
                s_fhist[bit / 8] |= (uint8_t)(1u << (bit % 8));
            else
                s_fhist[bit / 8] &= (uint8_t)~(1u << (bit % 8));
        }
    }
    /* After a gesture fired / settings closed, the finger may still be down:
     * swallow the whole touch until the next clean release so the renderer
     * never sees a stray down/up click. */
    if (s_swallow_release) {
        if (point_num == 0) s_swallow_release = 0;
        return;
    }
    if (point_num > 0) {
        s_touch_pts++;
        s_tx = x[0]; s_ty = y[0];
        uint32_t px, py;
        touch_to_page(x[0], y[0], &px, &py);
        if (!s_touch_down) {
            s_touch_down = 1;
            s_press_t0 = esp_timer_get_time() / 1000;
            s_press_x = x[0]; s_press_y = y[0];
            s_press_moved = 0;
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                "{\"type\":\"input\",\"kind\":\"down\",\"x\":%u,\"y\":%u,\"button\":\"left\"}",
                (unsigned)px, (unsigned)py);
            if (ws_send_locked(msg, (size_t)n) == 0) s_input_sent++;
        } else {
            /* long-press gesture: 3 s held within ±30 px opens settings */
            if ((int)(x[0] - s_press_x) > GESTURE_MOVE_TOL || (int)(x[0] - s_press_x) < -GESTURE_MOVE_TOL ||
                (int)(y[0] - s_press_y) > GESTURE_MOVE_TOL || (int)(y[0] - s_press_y) < -GESTURE_MOVE_TOL)
                s_press_moved = 1;
            if (!s_gesture_cfg && !s_press_moved &&
                esp_timer_get_time() / 1000 - s_press_t0 >= GESTURE_HOLD_MS) {
                s_gesture_cfg = 1;
                s_gesture_fires++;
                s_swallow_release = 1;
                ESP_LOGI(RTAG, "3 s long-press (no move) — opening settings screen");
                xTaskCreate(cfg_open_worker, "cfg", 12288, NULL, 5, NULL);
            }
            static int64_t last_mv = 0;
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_mv >= 10) {
                last_mv = now;
                char msg[96];
                int n = snprintf(msg, sizeof(msg),
                    "{\"type\":\"input\",\"kind\":\"move\",\"x\":%u,\"y\":%u,\"button\":\"none\"}",
                    (unsigned)px, (unsigned)py);
                ws_send_locked(msg, (size_t)n);
            }
        }
    } else {
        if (s_touch_down) {
            s_touch_down = 0;
            s_press_n++;
            s_press_last_ms = (uint32_t)(esp_timer_get_time() / 1000 - s_press_t0);
            if (s_press_last_ms > s_press_max_ms) s_press_max_ms = s_press_last_ms;
            if (s_gesture_cfg) {
                s_gesture_cfg = 0;   /* no "up" — the settings screen ate the press */
            } else {
                uint32_t px, py;
                touch_to_page(s_tx, s_ty, &px, &py);
                char msg[96];
                int n = snprintf(msg, sizeof(msg),
                    "{\"type\":\"input\",\"kind\":\"up\",\"x\":%u,\"y\":%u,\"button\":\"none\"}",
                    (unsigned)px, (unsigned)py);
                if (ws_send_locked(msg, (size_t)n) == 0) s_input_sent++;
            }
        }
    }
}

/* Dedicated touch task: polls the touch controller at ~200 Hz, independent of
 * the frame-decode loop. The decode loop blocks ~200-270ms per frame, so calling
 * touch_poll() from it starves touch to ~1.5 calls/sec and a fast tap can fall
 * entirely in a gap (touch feels dead). A 5 ms poll here means a tap held for
 * >50 ms is always caught, and it's never blocked by frame decode. Sends go
 * through ws_send_locked so they don't interleave with the remote_task's ping. */
static void touch_task(void *arg)
{
    (void)arg;
    s_touch_task_alive = true;
    ESP_LOGI(RTAG, "touch task up");
    for (;;) {
        if (s_remote_stop) break;
        touch_poll();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_touch_task_alive = false;
    ESP_LOGI(RTAG, "touch task down");
    vTaskDelete(NULL);
}

/* ============================ main task ============================ */

static void remote_task(void *arg)
{
    (void)arg;
    s_remote_task_alive = true;
    cfg_load();   /* NVS endpoint (settings screen) — defaults if never saved */
    ESP_LOGI(RTAG, "DBG-RT: step0 top core=%d", (int)xPortGetCoreID());
    ESP_LOGI(RTAG, "DBG-RT: ENTRY (renderer %s:%d url=%s)", s_cfg_host, s_cfg_port, s_cfg_url);

    extern esp_netif_t *app_sta_netif(void);
    esp_netif_t *sta = app_sta_netif();
    ESP_LOGI(RTAG, "DBG-RT: step1 pre-netif-wait sta=%p", (void *)sta);
    int waited_ms = 0;
    while (!sta && waited_ms < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
        sta = app_sta_netif();
        if (waited_ms % 2000 == 0)
            ESP_LOGI(RTAG, "DBG-RT: step2 netif-wait waited=%d sta=%p", waited_ms, (void *)sta);
    }
    ESP_LOGI(RTAG, "DBG-RT: step3 netif done (netif=%p, waited %d ms)", (void *)sta, waited_ms);
    vTaskDelay(pdMS_TO_TICKS(1000));

    s_fb0 = remote_fb_addr();
    ESP_LOGI(RTAG, "DBG-RT: step4 s_fb0=%p", (void *)s_fb0);
    if (!s_fb0) {
        ESP_LOGE(RTAG, "no framebuffer — remote off");
        snprintf(s_last_err, sizeof(s_last_err), "no fb");
        s_remote_task_alive = false;
        return;
    }
    s_jpeg_in = heap_caps_malloc(1200 * 1024, MALLOC_CAP_SPIRAM);
    s_rx      = heap_caps_malloc(1400 * 1024, MALLOC_CAP_SPIRAM);
    s_jpeg_in_sz = 1200 * 1024;
    s_rx_sz      = 1400 * 1024;
    ESP_LOGI(RTAG, "DBG-RT: step5 j=%p r=%p", (void *)s_jpeg_in, (void *)s_rx);
    if (!s_jpeg_in || !s_rx) {
        ESP_LOGE(RTAG, "buffer alloc failed (j=%p r=%p)", (void *)s_jpeg_in, (void *)s_rx);
        snprintf(s_last_err, sizeof(s_last_err), "alloc");
        s_remote_task_alive = false;
        return;
    }
    jpeg_decode_memory_alloc_cfg_t fb1_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t fb1_sz = 0;
    s_fb1 = jpeg_alloc_decoder_mem(RGB888_BYTES, &fb1_cfg, &fb1_sz);   /* RGB888 = 3B/px */
    ESP_LOGI(RTAG, "DBG-RT: step6 fb1=%p sz=%u", (void *)s_fb1, (unsigned)fb1_sz);
    ESP_LOGI(RTAG, "buffers ok fb=%p j=%p r=%p tmp=%p (tmpsz=%u) — switching UI",
             (void *)s_fb0, (void *)s_jpeg_in, (void *)s_rx, (void *)s_fb1, (unsigned)fb1_sz);
    /* Switch the display over to remote mode: hide the text UI + gate LVGL. */
    remote_ui_ready();
    ESP_LOGI(RTAG, "DBG-RT: step7 remote_ui_ready done");
    ESP_LOGI(RTAG, "remote UI active — waiting for renderer frames");

    /* Start the boot watchdog clock at "remote mode active" and launch it. If
     * no frame lands within the grace window (renderer or Wi-Fi down), it shows
     * the config screen instead of leaving a dead black panel. 12 KB unpinned,
     * prio 5 — identical to cfg_open_worker, which runs the same LVGL build. */
    s_boot_hint_open = false;
    s_boot_ref_ms    = esp_timer_get_time() / 1000;
    xTaskCreate(boot_watchdog, "bootwd", 12288, NULL, 5, NULL);

    for (;;) {
        s_connect_attempts++;
        ESP_LOGI(RTAG, "connect attempt %d", s_connect_attempts);
        if (s_connect_attempts == 2) {
            /* One-shot path probe (only on attempt 2, so it runs once per
             * session): can we even reach the NAS? 80 = portal (board's own
             * LAN path), 9100 = renderer, Homey 5050 = the render target. */
            probe_path();
        }
        if (ws_connect() != 0) {
            ESP_LOGW(RTAG, "connect failed: %s — retry in 3s", s_last_err);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        s_reconnects++;
        ESP_LOGI(RTAG, "connected (attempt %d)", s_connect_attempts);
        char conn[256];
        int cn = snprintf(conn, sizeof(conn),
            "{\"type\":\"connect\",\"url\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":12,\"quality\":60,\"bw\":%d,\"bh\":%d}",
            s_cfg_url, PAGE_W, PAGE_H, PAGE_W, PAGE_H);
        if (ws_send_locked(conn, (size_t)cn) != 0) {
            close(s_sock); s_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        s_remote_active = true;

        int64_t last_ping = 0;
        s_last_msg_ms = esp_timer_get_time() / 1000;   /* connection start = "last msg" → 30s grace before the renderer's first frame/tick lands */
        s_last_frame_ms = s_last_msg_ms;                /* connection start = "last frame" → 30s grace before the renderer's first frame lands (Chrome launch takes ~5-10s) */
        s_last_pong_ms  = s_last_msg_ms;                /* connection start = "last pong" → 40s grace before the pong timeout fires (first ping/pong round-trip lands ~immediately) */
        for (;;) {
            /* touch is handled by the dedicated touch task (see below) — NOT here.
             * Frame DECODE also runs in the dedicated present task (v6): this
             * loop only drains the socket, so pongs/ticks are never queued
             * behind a ~250 ms decode. */
            int64_t now = esp_timer_get_time() / 1000;
            /* Ping payload length MUST be the exact JSON length (15) — NOT the
             * strlen+1 (16). Sending the null terminator makes the renderer's
             * JSON.parse throw, so no pong is ever returned and the pong timeout
             * (40s) forces a perpetual reconnect (the recurring "flapping"). */
            if (now - last_ping > 20000) { last_ping = now; ws_send_locked("{\"type\":\"ping\"}", 15); }
            size_t flen = 0;
            int rf = ws_read_frame(s_sock, s_rx, s_rx_sz, &flen);
            if (rf > 0) s_last_msg_ms = esp_timer_get_time() / 1000;   /* ANY inbound msg (frame/hello/nav/tick) proves the receive direction is alive */
            if (rf == -2) {
                /* RCVTIMEO: idle. Two failure modes, both force a reconnect:
                 *  1. No inbound message of ANY type for 30s — the socket is
                 *     half-open (we can still send pings but the renderer->board
                 *     direction is dead). recv only ever times out (never errors),
                 *     so without this we loop forever on a dead connection.
                 *  2. No FRAME for 30s — the socket is alive (we still get the
                 *     5s ticks) but the renderer's Chrome instance is stale/wedged
                 *     (e.g. the board rebooted and the old client's browser is
                 *     frozen), so frames stopped flowing. The tick alone would
                 *     keep a frozen screen connected forever, so we track frames
                 *     separately and reconnect to force a fresh renderer. */
                int64_t nowms = esp_timer_get_time() / 1000;
                int64_t silent = nowms - s_last_msg_ms;
                int64_t frams  = nowms - s_last_frame_ms;
                if (silent > 30000) {
                    ESP_LOGW(RTAG, "no inbound msg for %lld ms — half-open socket, reconnecting", (long long)silent);
                    snprintf(s_last_err, sizeof(s_last_err), "silent %lldms", (long long)silent);
                    break;
                }
                if (frams > 30000) {
                    ESP_LOGW(RTAG, "no frame for %lld ms — frames not flowing, reconnecting", (long long)frams);
                    snprintf(s_last_err, sizeof(s_last_err), "noframe %lldms", (long long)frams);
                    break;
                }
                continue;
            }
            if (rf < 0) { ESP_LOGW(RTAG, "ws disconnected — reconnecting"); break; }
            if (rf == 0) continue;
            char *json = (char *)s_rx;
            json[flen < s_rx_sz ? flen : s_rx_sz - 1] = 0;
            if (strstr(json, "\"frame\"")) {
                /* Settings screen up: LVGL owns the framebuffer again — skip
                 * decode/present, keep counting (the connection stays warm). */
                if (!settings_active() && s_buf_free) {
                    /* Hand the frame to the present task via the single-slot
                     * staging buffer. If the presenter still holds the buffer
                     * (mid-decode), take(0) fails and this frame is DROPPED —
                     * the renderer re-sends within ~125 ms, so the cost is one
                     * skipped frame, not a stall. */
                    if (xSemaphoreTake(s_buf_free, 0) == pdTRUE) {
                        size_t jlen = extract_jpeg(json, s_jpeg_in, s_jpeg_in_sz);
                        if (jlen) {
                            s_frame_len = jlen;
                            xSemaphoreGive(s_frame_evt);
                        } else {
                            xSemaphoreGive(s_buf_free);   /* no JPEG here — hand back */
                        }
                    }
                }
                s_frames_rx++;
            } else if (strstr(json, "\"hello\"") || strstr(json, "\"nav\"")) {
                const char *tp = strstr(json, "\"title\":\"");
                if (tp) {
                    tp += 9;
                    const char *te = strchr(tp, '"');
                    if (te) {
                        size_t tl = (size_t)(te - tp);
                        if (tl >= sizeof(s_title)) tl = sizeof(s_title) - 1;
                        memcpy(s_title, tp, tl);
                        s_title[tl] = 0;
                    }
                }
                ESP_LOGI(RTAG, "server: %.*s", (int)(flen > 160 ? 160 : flen), json);
            } else if (strstr(json, "\"err\"")) {
                ESP_LOGW(RTAG, "renderer err: %.*s", (int)(flen > 160 ? 160 : flen), json);
                const char *mp = strstr(json, "\"msg\":\"");
                if (mp) {
                    mp += 7;
                    const char *me = strchr(mp, '"');
                    if (me) {
                        size_t ml = (size_t)(me - mp);
                        if (ml >= sizeof(s_last_err)) ml = sizeof(s_last_err) - 1;
                        memcpy(s_last_err, mp, ml);
                        s_last_err[ml] = 0;
                    }
                }
            } else if (strstr(json, "\"pong\"")) {
                s_last_pong_ms = esp_timer_get_time() / 1000;
            }
            /* PONG TIMEOUT — the ONLY detector for the one-directional
             * half-open state: renderer->board still flows (inbound frames keep
             * refreshing s_last_msg_ms/s_last_frame_ms) but board->renderer is
             * dead, so our pings/touch are dropped and no pong returns. The
             * silent- and no-frame checks both stay fresh from inbound frames,
             * so neither fires. 40s = two missed 20s ping/pong round-trips. */
            if (s_last_pong_ms > 0 && (esp_timer_get_time() / 1000 - s_last_pong_ms) > 40000) {
                int64_t pong_age = esp_timer_get_time() / 1000 - s_last_pong_ms;
                ESP_LOGW(RTAG, "no pong for %lld ms — board->renderer dead, reconnecting", (long long)pong_age);
                snprintf(s_last_err, sizeof(s_last_err), "npong %lldms", (long long)pong_age);
                break;
            }
        }
        s_remote_active = false;
        if (s_sock >= 0) { close(s_sock); s_sock = -1; }
        if (s_remote_stop) {
            /* Clean shutdown (settings screen changed the endpoint): release
             * the PSRAM buffers so remote_start() can re-allocate them fresh. */
            if (s_jpeg_in) { heap_caps_free(s_jpeg_in); s_jpeg_in = NULL; }
            if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
            if (s_fb1) { heap_caps_free(s_fb1); s_fb1 = NULL; }
            s_remote_task_alive = false;
            ESP_LOGI(RTAG, "remote task stopped (config change)");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ============================ public API (called from browser.c / main.c) ============================ */

void remote_start(void)
{
    /* Re-entry guard: a stop still in flight (settings save) or old tasks that
     * haven't exited yet — wait a moment, then refuse to double-start. */
    for (int i = 0; i < 500 && s_remote_stop; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_remote_task_alive || s_touch_task_alive || s_present_task_alive) {
        ESP_LOGW(RTAG, "remote_start: tasks still running — ignoring");
        return;
    }
    /* Capture heap state at entry — xTaskCreate(remote_task) was blocking here,
     * so this tells us which heap (internal vs PSRAM) is exhausted. */
    s_heap_int_kb   = (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    s_heap_psram_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    /* WS-send mutex: guards the shared socket for the remote_task (connect/ping)
     * and the touch task (input events). Must exist before either task sends. */
    if (!s_ws_send_lock) s_ws_send_lock = xSemaphoreCreateMutex();
    /* Present-pipeline sync objects: recreate, or reset to a known state on a
     * restart — the buffer must be FREE (count 1) and no frame may be staged
     * (a stale evt would decode a freshly re-allocated, uninitialized buffer). */
    if (!s_buf_free)  s_buf_free  = xSemaphoreCreateCounting(1, 1);
    if (!s_frame_evt) s_frame_evt = xSemaphoreCreateCounting(1, 0);
    while (xSemaphoreTake(s_frame_evt, 0) == pdTRUE) { /* drain stale frames */ }
    while (xSemaphoreTake(s_buf_free, 0) == pdTRUE) {  /* drain */ }
    xSemaphoreGive(s_buf_free);                        /* buffer is free */
    s_remote_stop = false;
    /* Create remote_task FIRST, before the touch task. The main task is pinned
     * to core 0 at prio 1; the moment touch_task (prio 2, core 0) is created it
     * preempts the main task, and together with c6wifi (prio 5, core 0) it
     * starves the main task out of the rest of remote_start() — xTaskCreate
     * (remote_task) then never gets a CPU slice to complete, so the remote task
     * was never created (rc stayed -99, attempts 0). Creating remote_task while
     * only c6wifi contends (the build-102 condition) lets the main task finish
     * its work, exactly as build 102 did. The touch task is created afterwards.
     *
     * Stack: 8 KB, not 32 KB. remote_task only ever uses ~200 bytes (the JPEG
     * decoder allocates its own heap/PSRAM, not the stack); 8 KB leaves
     * headroom next to the other core-1 stacks. */
    BaseType_t rc = xTaskCreatePinnedToCore(remote_task, "remote", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(present_task, "present", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 2, NULL, 0);
    s_remote_task_rc = (int)rc;
    s_remote_task_n = (int)uxTaskGetNumberOfTasks();
    s_remote_heap_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
    ESP_LOGI(RTAG, "DBG: xTaskCreate remote_task rc=%d heap=%u", (int)rc,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    if (rc != pdPASS) {
        ESP_LOGE(RTAG, "remote_task FAILED to create (rc=%d) — remote UI disabled", (int)rc);
    }
}

/* Stop the remote + touch tasks and release their PSRAM buffers. Waits up to
 * ~3 s for exit. Safe to call from any task (settings screen, httpd). */
void remote_stop(void)
{
    if (!s_remote_task_alive && !s_touch_task_alive && !s_present_task_alive) {
        s_remote_stop = false;
        return;
    }
    ESP_LOGI(RTAG, "remote_stop: requested");
    s_remote_stop = true;
    /* The present task owns s_jpeg_in while decoding — it MUST be out before
     * remote_task's shutdown path frees the PSRAM buffers. It notices
     * s_remote_stop at each 500 ms evt-timeout, so it's out within ~1.5 s. */
    for (int i = 0; i < 200 && s_present_task_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }   /* wake the blocked recv */
    for (int i = 0; i < 300 && s_remote_task_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 100 && s_touch_task_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_remote_active = false;
    s_remote_stop = false;
    ESP_LOGI(RTAG, "remote_stop: done");
}

/* Called after the settings screen closes: swallow the gesture finger's
 * release (and any lingering press) so the dashboard gets no stray click. */
void remote_touch_reset(void)
{
    s_swallow_release = 1;
    s_touch_down = 0;
    s_gesture_cfg = 0;
    s_press_t0 = 0;
}

/* Current renderer endpoint (NVS-backed; settings screen + /state). */
void remote_cfg_get(char *host, int host_len, int *port, char *url, int url_len)
{
    if (host && host_len) strlcpy(host, s_cfg_host, (size_t)host_len);
    if (port) *port = s_cfg_port;
    if (url && url_len) strlcpy(url, s_cfg_url, (size_t)url_len);
}

/* Appended to the /state output. */
void remote_diag(char *buf, size_t len, size_t *o)
{
    int64_t age = s_last_frame_ms ? (esp_timer_get_time() / 1000) - s_last_frame_ms : -1;
    int64_t pongage = s_last_pong_ms ? (esp_timer_get_time() / 1000) - s_last_pong_ms : -1;
    /* Center-pixel of the live framebuffer (what the panel shows), as raw RGB565
     * + decoded RGB — eliminates the camera from color verification. */
    uint16_t centerpx = 0, q1px = 0, q3px = 0;
    if (s_fb0) {
        size_t ci = (size_t)(PANEL_H / 2) * PANEL_W + (PANEL_W / 2);
        centerpx = ((uint16_t *)s_fb0)[ci];
        size_t q1 = (size_t)(PANEL_H / 4) * PANEL_W + (PANEL_W / 4);   /* upper-left quarter */
        size_t q3 = (size_t)(3 * PANEL_H / 4) * PANEL_W + (3 * PANEL_W / 4); /* lower-right */
        q1px = ((uint16_t *)s_fb0)[q1];
        q3px = ((uint16_t *)s_fb0)[q3];
    }
    *o += snprintf(buf + *o, len - *o,
        "remote: active=%d attempts=%d frames_rx=%d decoded=%d failed=%d input_sent=%d touch_pts=%d tpv=%d lastpts=%d frame_age=%lldms pong_age=%lldms rot_ms=%d title='%s' err='%s' fb1=%p remap=%d order=%d raw888c=%d,%d,%d px_center=0x%04x(%02x%02x%02x) px_q1=0x%04x px_q3=0x%04x rc=%d nt=%d heap=%dKB int=%dKB psram=%dKB\n",
        (int)s_remote_active, s_connect_attempts, s_frames_rx, s_frames_decoded,
        s_frames_failed, s_input_sent, s_touch_pts, s_touch_tpvalid, s_touch_lastpts, (long long)age, (long long)pongage, s_rot_ms, s_title, s_last_err,
        (void *)s_fb1,
        (int)(s_color_fix && s_fb1),
        s_rgb888_order,
        (int)s_raw888_center[0], (int)s_raw888_center[1], (int)s_raw888_center[2],
        (unsigned)centerpx, (centerpx >> 8) & 0xF8, (centerpx >> 3) & 0xFC, (centerpx << 3) & 0xF8,
        (unsigned)q1px, (unsigned)q3px,
        s_remote_task_rc, s_remote_task_n, s_remote_heap_kb, s_heap_int_kb, s_heap_psram_kb);
    /* Long-press telemetry: press durations, raw-fallback count, and the 16 s
     * RAW finger-presence ring (oldest byte first; 1 bit = chip saw a finger). */
    {
        char fh[2 * (FHIST_BITS / 8) + 1];
        for (int i = 0; i < FHIST_BITS / 8; i++) {
            uint8_t b = s_fhist[(s_fhist_head + i) % (FHIST_BITS / 8)];
            snprintf(fh + 2 * i, 3, "%02x", b);
        }
        *o += snprintf(buf + *o, len - *o,
            "touchhold: n=%lu last=%lums max=%lums gfires=%lu fb=%lu raw=%u fhist16s=%s\n",
            (unsigned long)s_press_n, (unsigned long)s_press_last_ms,
            (unsigned long)s_press_max_ms, (unsigned long)s_gesture_fires,
            (unsigned long)esp_lcd_gsl3680_diag_raw_fallback_count(),
            (unsigned)esp_lcd_gsl3680_diag_raw_finger_num(), fh);
    }
}
