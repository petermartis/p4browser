/*
 * p4browser v3 — LVGL display port + text-mode web browser.
 *
 * Display: the MIPI-DSI DPI panel (800x1280 RGB565, 2-lane, DMA2D) is driven
 * by an LVGL 9 display. LVGL renders into its own PSRAM draw buffer (partial
 * mode, 2 buffers); the flush callback hands each dirty region to
 * esp_lcd_panel_draw_bitmap, which DMA2D-copies it into the panel frame buffer
 * and fires on_color_trans_done -> lv_display_flush_ready (ISR).
 *
 * Browser: the phone is the keyboard. GET /url?u=<url> on the board's LAN IP
 * queues a nav. browser_task (core 1) fetches with esp_http_client (built-in
 * root CA bundle), converts HTML -> plain text, and publishes the result to
 * the LVGL task. All LVGL object access is guarded by a mutex (LVGL is not
 * thread-safe).
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_gsl3680.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "settings.h"

static const char *BTAG = "browser";

#define PAGE_BUF_CAP   (256 * 1024)
#define TITLE_CAP      256
#define URL_CAP        256
#define DRAW_BUF_LINES 40
#define LVGL_TASK_DELAY_MIN_MS 5
#define LVGL_TASK_DELAY_MAX_MS 50

/* ---- touch (GSL3680 capacitive, I2C 0x40) ----
 * JC8012P4A1: SDA=GPIO7, SCL=GPIO8, RST=GPIO22, INT=GPIO21. Panel native
 * 800x1280. Orientation mirrors the display (mirror_x per the DSI init);
 * mirror_y per the vendor README. Verified against the camera; if touch is
 * offset, flip the relevant mirror bit here. */
#define TOUCH_SDA_GPIO   GPIO_NUM_7
#define TOUCH_SCL_GPIO   GPIO_NUM_8
#define TOUCH_RST_GPIO   GPIO_NUM_22
#define TOUCH_INT_GPIO   GPIO_NUM_21
/* I2C port: new-API master bus. Empirically on this P4 board the LEGACY
 * APB i2c driver (i2c_driver_install/I2C_NUM_0, as the vendor Arduino ref
 * uses) TIMES OUT on every transaction on GPIO7/8 (probe -> ESP_ERR_TIMEOUT,
 * 0xf0 round-trip -> 0), whereas the new-API bus communicates fine (probe OK,
 * 0xf0 round-trips 12345678). So the new-API is the only working transport
 * here; the vendor ref's legacy path is a P4/Arduino artifact we can't reuse. */
#define TOUCH_I2C_NUM    I2C_NUM_1

/* ---- display / LVGL state ---- */
static lv_display_t       *s_disp  = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static lv_obj_t           *s_title_lbl = NULL;
static lv_obj_t           *s_url_lbl   = NULL;
static lv_obj_t           *s_body      = NULL;   /* scroll container */
static lv_obj_t           *s_body_lbl  = NULL;   /* text label inside container */
static SemaphoreHandle_t   s_lvgl_lock = NULL;
static char                s_ip[32] = {0};
static bool                s_ui_ready = false;
static uint8_t            *s_fb0 = NULL;         /* panel frame buffer 0 (RGB565) */

/* ---- remote (Option B: NAS-rendered video) ----
 * Once the remote pipeline takes over, the HW JPEG decoder writes s_fb0
 * directly and this file's LVGL flush / touch read must stand down (the
 * driver's normal touch-read path has no mutex — one reader at a time). */
static volatile bool s_remote_ui_active = false;

/* remote.c */
extern void remote_diag(char *buf, size_t len, size_t *o);

/* ---- nav queue ---- */
typedef struct {
    char url[URL_CAP];
    char title[TITLE_CAP];
    int  err;
} nav_evt_t;
static QueueHandle_t s_nav_q = NULL;
static char *s_page = NULL;   /* HTML buffer (PSRAM) */

static int    s_ui_update_count = 0;
static size_t s_last_text_len   = 0;

/* ---- touch state ---- */
static i2c_master_bus_handle_t s_touch_bus  = NULL;   /* new-API I2C bus */
static esp_lcd_panel_io_handle_t s_touch_io  = NULL;
static esp_lcd_touch_handle_t    s_touch     = NULL;
static lv_indev_t               *s_touch_dev = NULL;
static lv_obj_t                *s_touch_lbl = NULL;   /* on-screen tap indicator */
static lv_obj_t                *s_click_btn = NULL;   /* real LVGL button (click test) */
static lv_obj_t                *s_click_lbl = NULL;   /* click counter label */
static lv_obj_t                *s_xhair     = NULL;   /* crosshair at last tap (mapping check) */
static uint32_t                 s_clicks    = 0;      /* native LVGL click count */
static bool     s_touch_present = false;   /* I2C probe succeeded */
static int      s_touch_inits   = 0;       /* successful driver inits */

/* I2C bus mutex — SHARED with the GSL3680 driver (declared extern there).
 * The touch I2C bus is in blocking mode (no per-transaction timeout), so any
 * two concurrent transactions on it wedge it. The driver's per-frame read and
 * this file's /state raw dump both hit the bus; serializing them here prevents
 * the first-real-touch hang. Declared non-static + extern'd in the driver. */
SemaphoreHandle_t gsl3680_i2c_mutex = NULL;
static uint16_t s_touch_last_x  = 0;
static uint16_t s_touch_last_y  = 0;
static int      s_touch_presses = 0;       /* count of down->release cycles */
static int      s_touch_releases = 0;      /* count of genuine releases (press->no finger) */
static int      s_touch_points  = 0;       /* raw contact events read */

/* ---- touch raw-dump debug (read independently of the driver in /state) ---- */
static i2c_master_dev_handle_t s_dbg_dev = NULL;  /* raw-read device handle */
static int      s_dbg_int_level = -1;      /* last INT pin (GPIO21) level */
static int      s_dbg_int_err   = -1;      /* last INT gpio_get_level err */
/* Continuous finger-count monitor: a periodic task reads 0x80 (byte 0 =
 * Finger_num) every 50 ms and latches the max seen since last /state, so a
 * physical tap is provably observable even if it happens between polls. */
static volatile int s_dbg_max_fingers = 0;   /* highest 0x80 byte seen */
static volatile int s_dbg_finger_events = 0; /* count of samples where 0x80 > 0 */
static uint8_t  s_dbg_raw[24]  = {0};      /* last raw 0x80..0x97 read */
static int      s_dbg_read_err  = 0;       /* last i2c_master_read err (0=ok) */
static int      s_dbg_raw_calls = 0;       /* how many raw dumps done */
static uint8_t  s_dbg_id[4]     = {0};     /* device ID 0xd0..0xd3 */
static int      s_dbg_id_err    = 0;
static uint8_t  s_dbg_f0w[4]    = {0};     /* 0xf0 written pattern */
static uint8_t  s_dbg_f0r[4]    = {0};     /* 0xf0 read-back (round-trip test) */
static int      s_dbg_f0_err    = 0;
static uint8_t  s_dbg_b0[4]     = {0};     /* 0xb0 fw version (expect 5a5a5a5a) */
static int      s_dbg_b0_err    = 0;

#define BSTATE_IDLE     0
#define BSTATE_FETCHING 1
static volatile int  s_task_state = BSTATE_IDLE;
static char          s_fetching_url[64] = {0};

/* ============================ HTML -> text ============================ */

static void out_append(char **dst, size_t *len, size_t cap, char c)
{
    if (*len + 1 < cap) (*dst)[(*len)++] = c;
}

static char *unescape_at(char **dst, size_t *len, size_t cap, const char *p)
{
    /* p points at '&'; returns the position just past the consumed entity
     * (or past the lone '&'). */
    const char *q = p + 1;
    int consumed = 0;
    while (*q && *q != ';' && (q - p) < 11) q++;
    if (*q == ';') {
        size_t n = q - p - 1;
        const char *v = p + 1;
        if (n == 4 && memcmp(v, "amp;", 4) == 0)      { out_append(dst, len, cap, '&');  consumed = n + 1; }
        else if (n == 3 && memcmp(v, "lt;", 3) == 0)  { out_append(dst, len, cap, '<');  consumed = n + 1; }
        else if (n == 3 && memcmp(v, "gt;", 3) == 0)  { out_append(dst, len, cap, '>');  consumed = n + 1; }
        else if (n == 5 && memcmp(v, "quot;", 5) == 0){ out_append(dst, len, cap, '"');  consumed = n + 1; }
        else if (n == 5 && memcmp(v, "apos;", 5) == 0){ out_append(dst, len, cap, '\''); consumed = n + 1; }
        else if (n == 5 && memcmp(v, "nbsp;", 5) == 0){ out_append(dst, len, cap, ' ');  consumed = n + 1; }
        else if (n > 2 && v[0] == '#') {
            long val;
            if (n > 3 && (v[1] == 'x' || v[1] == 'X')) val = strtol(v + 2, NULL, 16);
            else                                        val = strtol(v + 1, NULL, 10);
            if (val >= 32 && val < 127) { out_append(dst, len, cap, (char)val); consumed = n + 1; }
        }
    }
    if (!consumed) out_append(dst, len, cap, '&');
    return (char *)(p + consumed);
}

static int is_block_tag(const char *t, int n)
{
    static const char *blk[] = { "p","div","br","li","ul","ol","h1","h2","h3","h4","h5","h6",
                                 "tr","table","section","article","header","footer","nav",
                                 "main","aside","pre","blockquote","hr","form","dd","dt","dl" };
    if (n < 1 || n > 9) return 0;
    char tbuf[10] = {0};
    memcpy(tbuf, t, n);
    for (unsigned i = 0; i < sizeof(blk)/sizeof(blk[0]); i++)
        if (strlen(blk[i]) == n && memcmp(blk[i], tbuf, n) == 0) return 1;
    return 0;
}

static size_t html_to_text(char *buf, size_t cap)
{
    char *o = buf;
    size_t olen = 0;
    const char *p = buf;
    int in_script = 0, in_style = 0, in_title = 0, in_pre = 0;
    while (*p) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            int n = (int)(e - p - 1);
            char tag[12] = {0};
            int closing = 0;
            const char *tp = p + 1;
            if (*tp == '/') { closing = 1; tp++; }
            while (*tp && isspace((unsigned char)*tp)) tp++;
            /* Extract the tag name ONLY. Must stop at whitespace, '>' and '/'
             * or attribute-bearing / short tags get mangled: e.g. "</style>"
             * (6 inner chars) would become "style>" and fail the strcmp below,
             * so in_style was never cleared and ALL body text after the second
             * <style> was dropped (only the <title> survived -> 11-byte render). */
            for (int i = 0; i < n && i < 11; i++) {
                char ch = *tp;
                if (!ch || ch == '>' || ch == '/' || isspace((unsigned char)ch)) break;
                tag[i] = (char)tolower((unsigned char)ch);
                tp++;
            }
            tag[11] = 0;
            if (!closing) {
                if (!strcmp(tag, "script")) in_script = 1;
                else if (!strcmp(tag, "style")) in_style = 1;
                else if (!strcmp(tag, "title")) in_title = 1;
                else if (!strcmp(tag, "pre")) in_pre = 1;
                else if (is_block_tag(tag, (int)strlen(tag))) out_append(&o, &olen, cap, '\n');
            } else {
                if (!strcmp(tag, "script")) in_script = 0;
                else if (!strcmp(tag, "style")) in_style = 0;
                else if (!strcmp(tag, "title")) in_title = 0;
                else if (!strcmp(tag, "pre")) in_pre = 0;
                else if (is_block_tag(tag, (int)strlen(tag))) out_append(&o, &olen, cap, '\n');
            }
            p = e + 1;
            continue;
        }
        if (in_script || in_style) { p++; continue; }
        char c = *p++;
        if (in_title) {
            if (c >= 32 && c < 127) out_append(&o, &olen, cap, c);
            continue;
        }
        if (c == '&') { p = unescape_at(&o, &olen, cap, p - 1) + 1; continue; }
        if (c == '\n' || c == '\r') {
            out_append(&o, &olen, cap, in_pre ? '\n' : ' ');
            continue;
        }
        if (c == '\t') c = ' ';
        if (c >= 32 && c < 127) out_append(&o, &olen, cap, c);
    }
    char *q = buf; size_t qlen = 0; int blanks = 0;
    for (size_t i = 0; i < olen; i++) {
        char c = buf[i];
        if (c == '\n') { blanks++; if (blanks <= 2) out_append(&q, &qlen, cap, '\n'); }
        else if (c == ' ') { if (blanks == 0 && qlen > 0 && q[qlen-1] != ' ') out_append(&q, &qlen, cap, ' '); }
        else { blanks = 0; out_append(&q, &qlen, cap, c); }
    }
    return qlen;
}

static int get_title(const char *html, char *out, size_t cap)
{
    const char *t = strstr(html, "<title");
    if (!t) return 0;
    t = strchr(t, '>');
    if (!t) return 0;
    t++;
    const char *e = strstr(t, "</title");
    if (!e) return 0;
    size_t n = e - t;
    char tmp[512] = {0};
    if (n < sizeof(tmp)) {
        memcpy(tmp, t, n); tmp[n] = 0;
        size_t tl = 0;
        (void)unescape_at((char **)&out, &tl, cap, tmp);
        out[tl] = 0;
        return 1;
    }
    return 0;
}

/* ============================ fetch ============================ */

/* perform() drains the body via the event handler (cache_data_in_fetch_hdr
 * is disabled around fetch_headers), so the body MUST be captured here —
 * esp_http_client_read() afterwards always returns 0. */
static char *s_body_cap = NULL;
static size_t s_body_cap_sz = 0;
static size_t s_body_len = 0;
static int s_body_err = 0;
static volatile int s_progress = 0;   /* bumped on every HTTP event (hang detector) */
/* Redirect-hop bookkeeping. esp_http_client_perform() follows every 3xx in a
 * single call and dispatches ON_DATA for each hop's body, so we must keep only
 * the FINAL response's body. HEADERS_SENT is dispatched once per request
 * (i.e. once per hop, in request_send) BEFORE that hop's body streams, so it is
 * the reliable per-hop reset point. s_hop_n counts hops in the current
 * perform() call (1 = no redirect happened). The final hop's status code is
 * read via esp_http_client_get_status_code() after perform() returns. */
static volatile int s_hop_n = 0;
/* Final response status code of the last http_get() (read via
 * esp_http_client_get_status_code after perform()). 0 = never fetched. */
static volatile int s_last_code = 0;

static int on_http_event(esp_http_client_event_t *evt)
{
    s_progress++;
    if (evt->event_id == HTTP_EVENT_HEADERS_SENT) {
        /* New request = new redirect hop. Discard the previous hop's body so
         * only the final (usually 200) response is kept. Idempotent on the
         * first hop (buffer is already zeroed in http_get before perform). */
        s_body_len = 0;
        if (s_body_cap) s_body_cap[0] = 0;
        s_hop_n++;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_body_len + evt->data_len <= s_body_cap_sz - 1) {
            memcpy(s_body_cap + s_body_len, evt->data, evt->data_len);
            s_body_len += evt->data_len;
            s_body_cap[s_body_len] = 0;
        } else if (s_body_len < s_body_cap_sz - 1) {
            size_t n = s_body_cap_sz - 1 - s_body_len;
            memcpy(s_body_cap + s_body_len, evt->data, n);
            s_body_len = s_body_cap_sz - 1;
            s_body_cap[s_body_len] = 0;
            ESP_LOGW(BTAG, "page truncated at %zu bytes", s_body_len);
        }
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        s_body_err = 1;
        ESP_LOGW(BTAG, "http event error");
    } else {
        ESP_LOGI(BTAG, "http evt %d (progress=%d)", evt->event_id, s_progress);
    }
    return ESP_OK;
}

static int http_get(const char *url, char *buf, size_t cap, char *ct, size_t ct_cap)
{
    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 5000;            /* per-transport-op timeout so EAGAIN re-arms */
    cfg.max_redirection_count = 3;
    cfg.event_handler = on_http_event;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "User-Agent", "p4browser/1.0");
    s_body_cap = buf;
    s_body_cap_sz = cap;
    s_body_len = 0;
    s_body_err = 0;
    s_progress = 0;
    s_hop_n = 0;
    s_last_code = 0;
    buf[0] = 0;
    int rc = 0;
    /* perform() may return ESP_ERR_HTTP_EAGAIN when a transport op times out;
     * call it again. Watch for a total hang: no new events for 35 s = abort. */
    int64_t deadline = esp_timer_get_time() / 1000 + 35000;
    int last_prog = -1;
    int64_t last_prog_ms = esp_timer_get_time() / 1000;
    for (;;) {
        esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK || err != ESP_ERR_HTTP_EAGAIN) {
            if (err != ESP_OK) {
                ESP_LOGW(BTAG, "http perform %s (%s)", url, esp_err_to_name(err));
                rc = -1;
            }
            break;
        }
        if (s_progress != last_prog) {
            last_prog = s_progress;
            last_prog_ms = esp_timer_get_time() / 1000;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now > deadline || (now - last_prog_ms) > 35000) {
            ESP_LOGW(BTAG, "fetch %s HANG ABORT: progress=%d (last evt logged above)", url, s_progress);
            rc = -4;
            break;
        }
    }
    if (rc == 0 && s_body_err) {
        ESP_LOGW(BTAG, "http event error during %s", url);
        rc = -1;
    }
    if (rc == 0) {
        long code = esp_http_client_get_status_code(c);
        s_last_code = (int)code;
        /* Accept 2xx success (esp_http_client follows redirects, so a healthy
         * fetch ends on the final 200). Everything else is a failure: 3xx means
         * the redirect chain didn't resolve (or exceeded max_redirection_count),
         * 4xx/5xx is an HTTP error. (The old check `code != 200 && code < 300`
         * only rejected non-200 in [100,300), so 3xx/4xx/5xx slipped through
         * and the partial redirect body got rendered as a page.) */
        int ok = (code >= 200 && code <= 299);
        if (!ok) {
            ESP_LOGW(BTAG, "http %ld for %s (hops=%d, body=%d B)", code, url,
                     s_hop_n, (int)s_body_len);
            rc = -2;   /* -2 = non-2xx final status */
        } else {
            char *ctv = NULL;
            if (esp_http_client_get_header(c, "Content-Type", &ctv) == ESP_OK && ctv)
                strlcpy(ct, ctv, ct_cap);
            ESP_LOGI(BTAG, "fetched %s: %d bytes, code=%ld hops=%d, type=%s",
                     url, (int)s_body_len, code, s_hop_n, ct[0] ? ct : "(none)");
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

static int ct_is_html(const char *ct)
{
    /* true if the content-type is HTML-ish: starts with "text/" or contains
     * "html" (case-insensitive). */
    if (strncmp(ct, "text/", 5) == 0) return 1;
    return strcasestr(ct, "html") != NULL;
}

static void ensure_scheme(char *u)
{
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
        memmove(u + 8, u, strlen(u) + 1);
        memcpy(u, "https://", 8);
    }
}

/* ============================ LVGL UI glue (call under s_lvgl_lock) ============================ */

/* First-flush-after-UI-change flag: when set, lvgl_flush_cb invalidates the
 * whole fbs[0] L2 before the first row of the new frame is written, so the
 * DSI GDMA (reading fbs[0] through the P4 L2) re-fetches fresh PSRAM instead
 * of hitting stale valid lines. Armed in browser_ui_update(). */
static volatile int s_new_frame = 1;

void browser_ui_update(const char *url, const char *title, const char *text, int err)
{
    int wait_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    int waited = xTaskGetTickCount() * portTICK_PERIOD_MS - wait_ms;
    s_ui_update_count++;
    s_last_text_len = (text && !err) ? strlen(text) : 0;
    ESP_LOGI(BTAG, "ui_update #%d waited=%dms text=%zuB err=%d",
             s_ui_update_count, waited, s_last_text_len, err);
    if (s_url_lbl) lv_label_set_text(s_url_lbl, url ? url : "");
    if (err) {
        if (s_title_lbl) lv_label_set_text(s_title_lbl, "error");
        if (s_body_lbl)  lv_label_set_text(s_body_lbl, text ? text : "unknown error");
    } else if (text) {
        if (s_title_lbl) lv_label_set_text(s_title_lbl, (title && title[0]) ? title : "(untitled)");
        if (s_body_lbl)  lv_label_set_text(s_body_lbl, text);
    }
    /* Arm the whole-frame L2 invalidate for the render this triggers: the
     * first flush of the new frame drops every stale boot line in fbs[0] so
     * the DSI GDMA (reading fbs[0] through the P4 L2) re-fetches fresh PSRAM.
     * Without this the GDMA keeps hitting the valid-but-stale boot lines. */
    s_new_frame = 1;
    xSemaphoreGive(s_lvgl_lock);
    ESP_LOGI(BTAG, "ui_update #%d done", s_ui_update_count);
}

void browser_ui_ip(const char *ip)
{
    if (ip) strlcpy(s_ip, ip, sizeof(s_ip));
}

/* ---- settings-screen support (settings.c) ----
 * The settings screen runs its LVGL object work from other tasks (httpd, the
 * remote touch task) under the same lock discipline as remote_ui_ready. */
void browser_lvgl_lock(void)   { xSemaphoreTake(s_lvgl_lock, portMAX_DELAY); }
void browser_lvgl_unlock(void) { xSemaphoreGive(s_lvgl_lock); }

/* Re-entrancy + timeout-aware lock helpers (settings.c). Two wedge classes
 * this board actually hit (2026-11-02): (1) an LVGL event callback (the
 * settings Back button) runs INSIDE lv_timer_handler, i.e. with the lock
 * already held by the lvgl task — a plain xSemaphoreTake there deadlocks
 * (FreeRTOS mutexes are not recursive); (2) a portMAX_DELAY take from a
 * non-LVGL task blocks forever when LVGL itself is spinning (the layout-spin
 * class) — which took down /state and /reboot with it. */
bool browser_lvgl_held(void)
{
    return s_lvgl_lock != NULL &&
           xSemaphoreGetMutexHolder(s_lvgl_lock) == xTaskGetCurrentTaskHandle();
}
bool browser_lvgl_trylock(uint32_t ms)
{
    if (browser_lvgl_held()) return true;   /* re-entrant: already ours */
    return s_lvgl_lock != NULL &&
           xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

/* Drop every L2 line for fbs[0] so the DSI GDMA re-fetches fresh PSRAM from
 * the line it next scans. Call whenever LVGL is about to own the screen again
 * (settings open/close) — without it the GDMA keeps replaying stale video
 * lines from its local cache on top of what LVGL draws. */
void browser_fb_invalidate(void)
{
    if (!s_fb0) return;
    esp_cache_msync(s_fb0, 800u * 1280u * 2u,
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* Current LAN IP (for /state and the settings screen). Timeout take: /state
 * must NEVER wedge on the LVGL lock — if LVGL is spinning it holds the lock
 * forever, and a portMAX_DELAY take here wedges the single httpd worker,
 * killing /state AND /reboot (observed 2026-11-02: settings-open wedge made
 * the board fully unreachable until power-cycle). */
void browser_ip_get(char *buf, size_t len)
{
    if (!buf || !len) return;
    if (xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        snprintf(buf, len, "-");
        return;
    }
    strlcpy(buf, s_ip, len);
    xSemaphoreGive(s_lvgl_lock);
}

/* ============================ LVGL display port ============================ */

/* ISR transfer-done counter (bumped in on_color_trans_done_lvgl) + two
 * diagnostic counters. Declared before lvgl_flush_cb which reads them. */
static volatile int s_xfer_done_count = 0;
/* Count of on_refresh_done callbacks — one per video frame the GDMA actually
 * streams fbs[0]->DSI->panel. If the DSI video stream is alive this rises at
 * ~dpi_clock/htotal/vtotal per second; if it's frozen (underrun / GDMA stopped)
 * it stays constant even though the framebuffer keeps getting written. */
static volatile int s_refresh_done_count = 0;
/* Count of flushes where the DPI driver reported the previous DMA2D transfer
 * still in flight (draw_sem not released) — the direct signature of a hung
 * or dropped transfer-done IRQ. */
static volatile int s_flush_stuck_count = 0;
/* Count of flush-wait timeouts (the ISR never signalled within the window). */
static volatile int s_flush_timeout_count = 0;
/* PSRAM scratch for rotating logical->physical pixels in the flush (the area
 * passed by LVGL is in the 1280x800 logical space; the panel needs the
 * 800x1280 fb orientation). Grows on demand, never shrinks. */
static uint8_t *s_rot_buf = NULL;
static size_t   s_rot_buf_sz = 0;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* Remote mode: the video path owns s_fb0 (the HW JPEG decoder decodes
     * straight into it). Never overwrite it with LVGL partial redraws —
     * complete the flush synchronously so the LVGL task isn't held waiting
     * for a transfer-done ISR that will never come.
     *
     * Exception: while the settings screen is up (settings.c), LVGL owns the
     * screen again — fall through to normal LVGL rendering. */
    if (s_remote_ui_active && !settings_active()) {
        lv_display_flush_ready(disp);
        return;
    }
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    static uint32_t n = 0;
    if (n < 3) {
        ESP_LOGI(BTAG, "flush #%d area (%ld,%ld)-(%ld,%ld) start", n,
                 (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2);
    }
    n++;
    /* FRAME START: drop every L2 line for fbs[0] so the GDMA's next video
     * read misses and pulls fresh data from PSRAM. No write-back (we must not
     * clobber anything; the rows that need writing get written back by
     * draw_bitmap's copy path). */
    if (s_new_frame && s_fb0) {
        s_new_frame = 0;
        esp_cache_msync(s_fb0, 800u * 1280u * 2u,
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    /* Rotated display (standard layout = landscape): LVGL passes the LOGICAL
     * area + logically-oriented pixels. Convert both — area via
     * lv_display_rotate_area, pixels via lv_draw_sw_rotate into a PSRAM
     * scratch buffer — then present the PHYSICAL area (draw_bitmap + the row
     * invalidation below stay unchanged). */
    lv_area_t phy = *area;
    uint8_t *present = px_map;
    if (lv_display_get_rotation(disp) != LV_DISPLAY_ROTATION_0) {
        lv_display_rotate_area(disp, &phy);
        uint32_t lw  = (uint32_t)lv_area_get_width(area);
        uint32_t lh  = (uint32_t)lv_area_get_height(area);
        uint32_t phw = (uint32_t)lv_area_get_width(&phy);
        size_t need = (size_t)lw * lh * sizeof(uint16_t);
        if (!s_rot_buf || s_rot_buf_sz < need) {
            uint8_t *nb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
            if (!nb) { s_flush_stuck_count++; lv_display_flush_ready(disp); return; }
            free(s_rot_buf);
            s_rot_buf = nb;
            s_rot_buf_sz = need;
        }
        lv_draw_sw_rotate(px_map, s_rot_buf, lw, lh, (int32_t)(lw * 2u),
                          (int32_t)(phw * 2u), lv_display_get_rotation(disp),
                          LV_COLOR_FORMAT_RGB565);
        present = s_rot_buf;
        area = &phy;
    }
    esp_err_t r = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, present);
    if (r != ESP_OK) s_flush_stuck_count++;
    /* Also invalidate just the rows we are about to present, so the GDMA does
     * not serve a stale line for them between now and the next frame boundary. */
    if (s_fb0) {
        uint16_t *fb = (uint16_t *)s_fb0;
        size_t row_bytes = 800u * 2u;
        uint8_t *start = (uint8_t *)(fb + area->y1 * 800);
        size_t span = (size_t)(area->y2 - area->y1 + 1) * row_bytes;
        esp_cache_msync(start, span, ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    if (n < 3) {
        ESP_LOGI(BTAG, "flush #%lu ret=%s (stuck=%d)", n - 1, esp_err_to_name(r), s_flush_stuck_count);
    }
}

/* DPI transfer-done -> signal LVGL flush ready. Runs in ISR context (DPI
 * driver async_fbcpy_done_cb) — NO logging, NO blocking calls here. Plain
 * (non-IRAM_ATTR) function is OK because CONFIG_LCD_DSI_ISR_IRAM_SAFE is OFF.
 * The lv_display_t is passed as user_ctx. */
static bool on_color_trans_done_lvgl(esp_lcd_panel_handle_t panel,
                                     esp_lcd_dpi_panel_event_data_t *edata,
                                     void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    s_xfer_done_count++;
    if (disp) lv_display_flush_ready(disp);
    return false;
}

/* Fires in the GDMA done ISR, once per video frame the DSI video stream
 * actually transmitted to the panel. ISR-safe: counter increment only. */
static bool on_refresh_done_cb(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    s_refresh_done_count++;
    return false;
}

/* LVGL flush-wait callback (task context, LV_OS_NONE). Replaces LVGL's
 * default bare busy-wait on disp->flushing: we poll the ISR transfer-done
 * counter for up to 5 s, then give up so a single missed DMA2D IRQ can never
 * wedge the LVGL task (and thus the whole UI + browser) forever. */
#define FLUSH_WAIT_TIMEOUT_MS 5000
static void lvgl_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    int base = s_xfer_done_count;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < (int64_t)FLUSH_WAIT_TIMEOUT_MS * 1000) {
        /* LVGL only calls us while flushing==1, and the DMA2D done ISR bumps
         * s_xfer_done_count atomically with clearing flushing — so the ISR for
         * this transfer hasn't fired yet; wait for the counter to move. */
        if (s_xfer_done_count != base) return;
        vTaskDelay(2);
    }
    s_flush_timeout_count++;
    ESP_LOGE(BTAG, "flush wait TIMEOUT after %d ms (xfer_done=%d) — forcing flush ready; "
                   "the DMA2D done IRQ never fired for this transfer",
             FLUSH_WAIT_TIMEOUT_MS, s_xfer_done_count);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(10);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(BTAG, "LVGL task up");
    uint32_t t = 0;
    for (;;) {
        xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
        t = lv_timer_handler();
        xSemaphoreGive(s_lvgl_lock);
        if (t < LVGL_TASK_DELAY_MIN_MS) t = LVGL_TASK_DELAY_MIN_MS;
        if (t > LVGL_TASK_DELAY_MAX_MS) t = LVGL_TASK_DELAY_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(t));
    }
}

/* ---- LVGL lock wedge watchdog (2026-11-02) ----
 * If ANY task holds s_lvgl_lock for >3 s, something is spinning inside LVGL
 * (a layout pass / object build / first render that never returns). Left
 * alone that wedges the whole board: /state and /reboot block on the lock,
 * the frame presenter and touch task starve, the display freezes — and this
 * board's serial console is dead, so there is no other recovery channel.
 * Rebooting self-heals: the renderer re-adopts the WS in ~2 s. The held-ms +
 * holder-task fields are exposed in /state ("lvlock:") so a wedge is
 * observable, not just survived. */
static volatile int64_t s_lock_held_ms = 0;
static char s_lock_holder[24] = "-";

static void lvgl_lock_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* lock was free: probe done, nothing was wedged */
            xSemaphoreGive(s_lvgl_lock);
            s_lock_held_ms = 0;
            strlcpy(s_lock_holder, "-", sizeof(s_lock_holder));
        } else {
            s_lock_held_ms += 500;
            TaskHandle_t h = xSemaphoreGetMutexHolder(s_lvgl_lock);
            const char *n = h ? pcTaskGetName(h) : NULL;
            strlcpy(s_lock_holder, n ? n : "?", sizeof(s_lock_holder));
            if (s_lock_held_ms >= 3000) {
                ESP_LOGE(BTAG, "LVGL lock held %lld ms by task '%s' — wedge detected, rebooting",
                         (long long)s_lock_held_ms, s_lock_holder);
                vTaskDelay(pdMS_TO_TICKS(100));   /* let the log flush out UART */
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* Native LVGL button click handler — the ground-truth touch test. A tap that
 * LVGL resolves into a click on this button bumps s_clicks. If this counts, the
 * full touch->LVGL->event->callback chain (what Homey device toggling needs)
 * works. */
static void on_click(lv_event_t *e)
{
    (void)e;
    s_clicks++;
    char t[48];
    snprintf(t, sizeof(t), "clicks: %lu", (unsigned long)s_clicks);
    if (s_click_lbl) lv_label_set_text(s_click_lbl, t);
    ESP_LOGI(BTAG, "LVGL CLICK #%lu (native)", (unsigned long)s_clicks);
}

/* PRESSED on the full-screen button: drop a crosshair at the EXACT reported
 * (x,y). Camera comparison of where the finger was vs where the crosshair lands
 * proves the coordinate mapping (mirror/rotate) is correct. */
static void on_pressed(lv_event_t *e)
{
    (void)e;
    if (s_xhair) {
        lv_obj_set_pos(s_xhair, s_touch_last_x - 40, s_touch_last_y - 40);
        lv_obj_remove_flag(s_xhair, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    /* title bar */
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_width(s_title_lbl, 1280 - 40);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(0x4FC3F7), LV_PART_MAIN);

    /* url bar */
    s_url_lbl = lv_label_create(scr);
    lv_obj_set_width(s_url_lbl, 1280 - 40);
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_url_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_url_lbl, LV_ALIGN_TOP_LEFT, 20, 100);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(0x9E9E9E), LV_PART_MAIN);

    /* scrollable body container (shrink to leave a bottom control strip) */
    s_body = lv_obj_create(scr);
    lv_obj_set_size(s_body, 1280, 800 - 260);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 140);
    lv_obj_set_style_bg_color(s_body, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_body, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_body, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLL_WITH_ARROW);

    /* text label inside the container */
    s_body_lbl = lv_label_create(s_body);
    lv_obj_set_width(s_body_lbl, 1280 - 40);
    lv_label_set_long_mode(s_body_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_body_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_body_lbl, LV_ALIGN_TOP_LEFT, 20, 10);
    lv_obj_set_style_text_line_space(s_body_lbl, 6, LV_PART_MAIN);

    const char *boot =
        "p4browser v3\n\n"
        "Type a URL on your phone, e.g.:\n\n"
        "  http://" "192.168.3.129/url?u=example.com\n\n"
        "(the real LAN IP is shown on boot)";
    lv_label_set_text(s_body_lbl, boot);
    lv_label_set_text(s_title_lbl, "p4browser");
    lv_label_set_text(s_url_lbl, "ready");

    /* On-screen touch indicator: a big label in a bottom strip showing the
     * running tap count + last touch coords, so a physical tap is visible to
     * the camera (the web page content itself has no touch-reactive UI). */
    s_touch_lbl = lv_label_create(scr);
    lv_obj_set_width(s_touch_lbl, 1280 - 20);
    lv_label_set_long_mode(s_touch_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_touch_lbl, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(s_touch_lbl, LV_ALIGN_BOTTOM_LEFT, 20, -60);
    lv_obj_set_style_text_color(s_touch_lbl, lv_color_hex(0xFFEB3B), LV_PART_MAIN);
    lv_obj_add_flag(s_touch_lbl, LV_OBJ_FLAG_FLOATING);
    lv_label_set_text(s_touch_lbl, "touch: 0 taps");

    /* Real LVGL button + click counter (ground-truth touch->LVGL test).
     * FULL-SCREEN so a tap anywhere hits it — removes coordinate mapping as a
     * variable. If clicks still stays 0, the cause is a missing LVGL release,
     * not a mapping error. */
    s_click_btn = lv_button_create(scr);
    lv_obj_set_size(s_click_btn, 1280, 800);
    lv_obj_align(s_click_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_click_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_click_btn, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_click_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_click_btn, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_click_btn, on_pressed, LV_EVENT_PRESSED, NULL);
    s_click_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_click_lbl, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(s_click_lbl, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_set_style_text_color(s_click_lbl, lv_color_hex(0x66FF66), LV_PART_MAIN);
    lv_obj_add_flag(s_click_lbl, LV_OBJ_FLAG_FLOATING);
    lv_label_set_text(s_click_lbl, "clicks: 0");

    /* Crosshair at last tap position (mapping check via camera). Two thin
     * bars forming a "+" 80x80, centered on the reported point by on_pressed. */
    s_xhair = lv_obj_create(scr);
    lv_obj_set_size(s_xhair, 80, 80);
    lv_obj_set_style_bg_opa(s_xhair, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_xhair, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_xhair, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_xhair, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_xhair, LV_OBJ_FLAG_CLICKABLE); /* don't block button events */
    {
        lv_obj_t *h = lv_obj_create(s_xhair);
        lv_obj_set_size(h, 80, 6);
        lv_obj_align(h, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(0xFF0000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *v = lv_obj_create(s_xhair);
        lv_obj_set_size(v, 6, 80);
        lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(v, lv_color_hex(0xFF0000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(v, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(v, 0, LV_PART_MAIN);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ============================ touch (GSL3680) ============================ */

/* LVGL indev read callback. Runs in the LVGL task (holds s_lvgl_lock).
 * Polls the GSL3680; returns press state + position. Coordinates are in the
 * touch controller's native space (x_max=800, y_max=1280) which already
 * matches the LVGL display size, so no scaling is needed here. */
static void touch_read_cb(lv_indev_t *dev, lv_indev_data_t *data)
{
    (void)dev;
    static bool was_pressed = false;
    if (!s_touch) { data->state = LV_INDEV_STATE_RELEASED; return; }
    /* Remote mode: remote.c is the sole touch reader (the driver's normal
     * read path is not mutex-serialized; two readers wedge the blocking I2C
     * bus). LVGL gets no touch input while video owns the screen — the touch
     * goes upstream to the renderer instead.
     *
     * Exception: while the settings screen is up, the remote touch task
     * stands down and LVGL is the sole reader — fall through to normal
     * touch handling. */
    if (s_remote_ui_active && !settings_active()) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        return;
    }
    uint16_t x[1], y[1], str[1];
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    if (!esp_lcd_touch_get_coordinates(s_touch, x, y, str, &count, 1)) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        return;
    }
    if (count > 0) {
        s_touch_points++;
        s_touch_last_x = x[0];
        s_touch_last_y = y[0];
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
        if (!was_pressed) {
            s_touch_presses++;
            char tl[64];
            snprintf(tl, sizeof(tl), "touch: %d taps  (%u,%u)",
                     s_touch_presses, x[0], y[0]);
            if (s_touch_lbl) lv_label_set_text(s_touch_lbl, tl);
        }
        was_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = s_touch_last_x;
        data->point.y = s_touch_last_y;
        if (was_pressed) {   /* genuine press->release edge */
            s_touch_releases++;
        }
        was_pressed = false;
    }
}

/* Bring up the touch controller. Runs in its own task (the GSL3680 driver
 * uploads firmware to the chip on init — ~800 ms with I2C delays — which
 * would risk the watchdog if done in app_main). The LVGL indev is registered
 * under s_lvgl_lock; everything else is single-threaded here. Returns ESP_OK
 * if a panel was found, ESP_ERR_NOT_FOUND if I2C probe failed (board then
 * runs without touch, exactly like before). */
/* No-op interrupt callback. The GSL3680 driver enables the GPIO21 interrupt
 * line unconditionally; if interrupt_callback is NULL the base driver never
 * installs an ISR handler (gpio_isr_handler_add), so the first INT assert
 * takes core 1 into the GPIO ISR with no handler -> Instruction access fault.
 * We poll for touches (touch_read_cb), so the ISR body is empty — it only has
 * to exist so the interrupt line is acknowledged and does not fault. */
static void touch_int_isr_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;
}

static esp_err_t touch_init(void)
{
    ESP_LOGI(BTAG, "touch: step1 new i2c bus (I2C1, SDA7/SCL8 @400k)");
    /* 1. New-API I2C master bus. Empirically the only transport that reaches
     * the chip on this P4 board (legacy APB driver times out on GPIO7/8). */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_NUM,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_touch_bus);
    if (err != ESP_OK) {
        ESP_LOGE(BTAG, "touch: i2c_new_master_bus failed (0x%x) — no touch", err);
        return err;
    }

    /* Bus mutex: serialize the driver's per-frame read against the /state raw
     * dump (both hit this blocking bus). Created before the driver inits so the
     * driver's extern'd handle is live for every subsequent transaction. */
    gsl3680_i2c_mutex = xSemaphoreCreateMutex();
    if (!gsl3680_i2c_mutex) {
        ESP_LOGW(BTAG, "touch: i2c mutex create failed — unserialized I2C (hang risk)");
    }

    /* 2. Pulse RST (low then high) so the controller is out of reset before
     * we probe — the constructor does its own reset later, this just makes
     * the probe reliable. */
    ESP_LOGI(BTAG, "touch: step2 rst pulse (GPIO%d)", (int)TOUCH_RST_GPIO);
    {
        gpio_config_t rc = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(TOUCH_RST_GPIO),
        };
        gpio_config(&rc);
        gpio_set_level(TOUCH_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(TOUCH_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 3. Probe for the controller so a board without a panel still boots. */
    ESP_LOGI(BTAG, "touch: step3 probe 0x40");
    err = i2c_master_probe(s_touch_bus, ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS, 100);
    if (err != ESP_OK) {
        ESP_LOGW(BTAG, "touch: no GSL3680 at 0x40 (probe 0x%x) — continuing without touch", err);
        i2c_del_master_bus(s_touch_bus);
        s_touch_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    s_touch_present = true;
    ESP_LOGI(BTAG, "touch: step4 panel io (probe OK)");

    /* Raw-read device handle for the /state touch debug dump (independent of
     * the driver's own panel-io path). */
    {
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS,
            .scl_speed_hz = 400 * 1000,
        };
        if (i2c_master_bus_add_device(s_touch_bus, &dcfg, &s_dbg_dev) != ESP_OK) {
            ESP_LOGW(BTAG, "touch: raw dev handle failed — /state dump unavailable");
            s_dbg_dev = NULL;
        }
    }

    /* 4. Panel IO over the new bus (bus handle -> _v2 path). The _v2 impl
     * honours io_config->scl_speed_hz, which the GSL3680 macro leaves 0, so
     * set it explicitly. */
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
    io_cfg.scl_speed_hz = 400 * 1000;
    err = esp_lcd_new_panel_io_i2c(s_touch_bus, &io_cfg, &s_touch_io);
    if (err != ESP_OK || !s_touch_io) {
        ESP_LOGE(BTAG, "touch: panel io init failed (0x%x) — no touch", err);
        s_touch_present = false;
        i2c_del_master_bus(s_touch_bus);
        s_touch_bus = NULL;
        return err;
    }
    ESP_LOGI(BTAG, "touch: step5 gsl3680 driver init (firmware upload ~800ms)");

    /* 5. Touch driver (firmware upload happens here, ~800 ms). */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 800,
        .y_max = 1280,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },   /* both active low */
        /* mirror_x=1 (2026-10-31): the touch panel reports x mirrored
         * relative to the physical panel (raw_x = 800 - phys_x). Without
         * this the remote-UI touch is vertically flipped (page_y = touch_x
         * is mirrored): tapping row 1 hits row 3 and vice versa. Camera +
         * full tap-log confirmed: page_y_actual = 800 - page_y_intended.
         * X-axis (page_x = 1279 - native_y) is NOT mirrored, so only
         * mirror_x is set. This also corrects the LVGL UI (same indev). */
        .flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 0 },
        .interrupt_callback = touch_int_isr_cb,   /* no-op: keeps the GPIO ISR alive (see above) */
    };
    err = esp_lcd_touch_new_i2c_gsl3680(s_touch_io, &tp_cfg, &s_touch);
    if (err != ESP_OK || !s_touch) {
        ESP_LOGE(BTAG, "touch: GSL3680 driver init failed (0x%x) — no touch", err);
        s_touch_present = false;
        if (s_touch_io) esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
        i2c_del_master_bus(s_touch_bus);
        s_touch_bus = NULL;
        return err;
    }
    s_touch_inits++;
    ESP_LOGI(BTAG, "touch: step6 GSL3680 ready — creating lv_indev");

    /* 6. LVGL indev (register under the LVGL lock; the display already
     * exists — touch_init runs after browser_display_init). */
    xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    s_touch_dev = lv_indev_create();
    if (s_touch_dev) {
        lv_indev_set_type(s_touch_dev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_dev, touch_read_cb);
    }
    xSemaphoreGive(s_lvgl_lock);
    ESP_LOGI(BTAG, "touch: step7 lv_indev created=%p", (void*)s_touch_dev);

    return s_touch_dev ? ESP_OK : ESP_FAIL;
}

esp_err_t browser_display_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    s_lvgl_lock = xSemaphoreCreateMutex();
    if (!s_lvgl_lock) return ESP_FAIL;

    lv_init();

    s_disp = lv_display_create(800, 1280);
    if (!s_disp) { ESP_LOGE(BTAG, "lv_display_create failed"); return ESP_FAIL; }
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    /* Standard layout = LANDSCAPE (1280x800 logical). The dashboard video is
     * already presented rotated into the 800x1280 fb by remote.c
     * (native(nx,ny) = page(px=1279-ny, py=nx)), so rotate the LVGL UI the
     * same way: LVGL keeps disp 800x1280 internally, the logical screen is
     * 1280x800, and lvgl_flush_cb converts each dirty area logical->physical.
     * Indev points (raw panel coords) are converted by LVGL automatically. */
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);

    size_t dbsz = 800 * DRAW_BUF_LINES * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_malloc(dbsz, MALLOC_CAP_SPIRAM);
    uint8_t *buf2 = heap_caps_malloc(dbsz, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) { ESP_LOGE(BTAG, "draw buffer alloc failed (%zu B)", dbsz); return ESP_ERR_NO_MEM; }
    lv_display_set_buffers(s_disp, buf1, buf2, dbsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    /* Bound the LVGL->DMA2D wait so a single missed transfer-done IRQ can't
     * wedge the UI (replaces LVGL's default unbounded busy-wait). */
    lv_display_set_flush_wait_cb(s_disp, lvgl_flush_wait_cb);

    /* Signal LVGL when the DMA2D colour transfer to the panel completes.
     * Pass the lv_display_t as user_ctx (the example's pattern). */
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done_lvgl,
        .on_refresh_done     = on_refresh_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, s_disp),
                        BTAG, "register lvgl flush cbs");

    build_ui();
    s_ui_ready = true;

    /* Grab the panel's single frame buffer (RGB565, 800x1280) so /state can
     * read back the pixels the DSI video path actually presents. With
     * num_fbs=1 this is the exact buffer the panel scans out. */
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&s_fb0) != ESP_OK || !s_fb0) {
        ESP_LOGW(BTAG, "get_frame_buffer failed — /state pixel readback unavailable");
    } else {
        ESP_LOGI(BTAG, "frame buffer 0 @ %p", (void *)s_fb0);
    }

    esp_timer_create_args_t tca = { .callback = lvgl_tick_cb, .name = "lvgl" };
    esp_timer_handle_t tick;
    if (esp_timer_create(&tca, &tick) == ESP_OK) esp_timer_start_periodic(tick, 10000);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(lvgl_lock_watchdog_task, "lvlockwd", 4096, NULL, 3, NULL, 1);
    return ESP_OK;
}

/* ============================ browser fetch task (core 1) ============================ */

static void browser_task(void *arg)
{
    ESP_LOGI(BTAG, "browser task up");
    for (;;) {
        nav_evt_t *ev = NULL;
        if (xQueueReceive(s_nav_q, &ev, portMAX_DELAY) != pdTRUE || !ev) continue;
        char url[URL_CAP] = {0}, title[TITLE_CAP] = {0};
        strlcpy(url, ev->url, sizeof(url));
        strlcpy(title, ev->title, sizeof(title));
        heap_caps_free(ev);

        ensure_scheme(url);
        ESP_LOGI(BTAG, "navigate: %s", url);
        s_task_state = BSTATE_FETCHING;
        strlcpy(s_fetching_url, url, sizeof(s_fetching_url));
        char ct[64] = {0};
        int rc = http_get(url, s_page, PAGE_BUF_CAP, ct, sizeof(ct));
        s_task_state = BSTATE_IDLE;
        s_fetching_url[0] = 0;
        if (rc != 0) {
            char m[160];
            snprintf(m, sizeof(m), "could not load\n%s\n\nhttp error %d", url, rc);
            browser_ui_update(url, NULL, m, 1);
            continue;
        }
        if (ct[0] && !ct_is_html(ct)) {
            char m[160];
            snprintf(m, sizeof(m), "unsupported type:\n%s\n\nv3 renders text/html only", ct);
            browser_ui_update(url, NULL, m, 1);
            continue;
        }
        if (get_title(s_page, title, sizeof(title)) == 0) title[0] = 0;
        /* html_to_text rewrites s_page in-place but does NOT NUL-terminate at
         * the (shorter) converted length — without this the old HTML bytes
         * beyond qlen remain, and lv_label_set_text's strlen() runs to the
         * stale fetch NUL (~100KB of garbage), making LVGL's wrap layout pass
         * spin forever while holding s_lvgl_lock (the original freeze). */
        size_t tlen = html_to_text(s_page, PAGE_BUF_CAP);
        if (tlen >= PAGE_BUF_CAP) tlen = PAGE_BUF_CAP - 1;
        s_page[tlen] = 0;
        browser_ui_update(url, title, s_page, 0);
    }
}

/* ============================ public API ============================ */

esp_err_t browser_init(void)
{
    s_nav_q = xQueueCreate(4, sizeof(nav_evt_t *));
    if (!s_nav_q) return ESP_FAIL;
    s_page = heap_caps_malloc(PAGE_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!s_page) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(browser_task, "browser", 16384, NULL, 4, NULL, 1);
    return ESP_OK;
}

/* Touch init task: the GSL3680 driver uploads firmware to the chip during
 * init (~800 ms of I2C + delays). Doing that in a dedicated low-priority task
 * keeps it off the boot critical path and away from the watchdog. The display
 * + LVGL lock must already exist (browser_display_init ran first), so we
 * delay one tick before starting. */
/* 50 ms 0x80 finger-count poll (debug). DISABLED: it shares the touch I2C bus
 * with the driver's per-frame read+write sequence (the driver also WRITES 0xf0
 * and 0x8 when a finger is present). On the first real touch the two collide on
 * the blocking bus and wedge -> whole board hangs with no panic/watchdog.
 * Re-enable only after adding a bus mutex that also covers the driver path. */
#define TOUCH_FINGER_MONITOR 0
#if TOUCH_FINGER_MONITOR
static void touch_finger_monitor_task(void *arg);  /* forward decl (defined later) */
#endif

static void touch_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(BTAG, "touch: task starting (core=%d)", (int)xPortGetCoreID());
    esp_err_t err = touch_init();
    if (err == ESP_OK) {
        ESP_LOGI(BTAG, "touch: indev live — tap the screen (count via /state)");
#if TOUCH_FINGER_MONITOR
        /* Continuous finger-count monitor: latches the max 0x80 byte seen so
         * a physical tap is provably observable in /state (max_fingers). */
        xTaskCreatePinnedToCore(touch_finger_monitor_task, "touchmon",
                                4096, NULL, 2, NULL, 1);
#endif
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(BTAG, "touch: not present — board runs without touch");
    } else {
        ESP_LOGE(BTAG, "touch: init failed (0x%x)", err);
    }
    /* Task exits after init. Builds 50-58 faulted ~1.3s after this exit;
     * the crash no longer reproduces on build 60 (frame-pointer build +
     * core-dump config) — ~30 min uptime verified. */
    vTaskDelete(NULL);
}

void browser_touch_start(void)
{
    xTaskCreatePinnedToCore(touch_task, "touch", 16384, NULL, 3, NULL, 1);
}

/* ============================ remote (Option B) wiring ============================ */

/* Touch handle for remote.c (the remote task becomes the sole touch reader
 * once remote mode is active). */
esp_lcd_touch_handle_t remote_touch_handle(void)
{
    return s_touch;
}

/* Panel frame buffer (RGB565, 800x1280) — remote.c's HW JPEG decoder writes
 * directly into this. */
void *remote_fb_addr(void)
{
    return s_fb0;
}

/* Called by remote.c once it owns the display. Hides the text UI + touch
 * overlay (the video is the whole screen now) and flips the gate so LVGL
 * flush + touch read stand down. Safe to call before s_ui_ready (no-op on
 * labels). */
void remote_ui_ready(void)
{
    xSemaphoreTake(s_lvgl_lock, portMAX_DELAY);
    s_remote_ui_active = true;
    if (s_title_lbl) lv_obj_add_flag(s_title_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_url_lbl)   lv_obj_add_flag(s_url_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_body)      lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    if (s_body_lbl)  lv_obj_add_flag(s_body_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_touch_lbl) lv_obj_add_flag(s_touch_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_click_btn) lv_obj_add_flag(s_click_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_click_lbl) lv_obj_add_flag(s_click_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_xhair)     lv_obj_add_flag(s_xhair, LV_OBJ_FLAG_HIDDEN);
    xSemaphoreGive(s_lvgl_lock);
    ESP_LOGI(BTAG, "remote UI active — text UI hidden, LVGL flush gated");
}

void browser_navigate(const char *url)
{
    if (!s_nav_q || !url) return;
    nav_evt_t *ev = heap_caps_calloc(1, sizeof(nav_evt_t), MALLOC_CAP_8BIT);
    if (!ev) return;
    strlcpy(ev->url, url, sizeof(ev->url));
    if (xQueueSend(s_nav_q, &ev, 0) != pdTRUE) heap_caps_free(ev);
}

/* ============================ diagnostics (/state) ============================ */

/* Independent raw read of the GSL3680 registers, straight off the I2C bus
 * (does NOT go through the driver's read_data / get_xy, so it reports what
 * the chip is ACTUALLY presenting). Reads the 24-byte touch data block
 * (0x80..0x97, byte 0 = finger count) and the device ID (0xd0..0xd3), and
 * samples the INT pin. Populated on every /state call. */
static void touch_raw_dump(void)
{
    s_dbg_int_level = gpio_get_level(TOUCH_INT_GPIO);
    if (!s_dbg_dev) { s_dbg_read_err = 0x8000; return; }

    /* Serialize with the driver's per-frame read (same blocking I2C bus). */
    bool have = false;
    if (gsl3680_i2c_mutex) have = xSemaphoreTake(gsl3680_i2c_mutex, pdMS_TO_TICKS(150)) == pdTRUE;

    uint8_t reg = 0x80;
    s_dbg_read_err = (int)i2c_master_transmit_receive(s_dbg_dev,
                          &reg, 1, s_dbg_raw, sizeof(s_dbg_raw), 50);
    s_dbg_raw_calls++;

    uint8_t idreg = 0xd0;
    s_dbg_id_err = (int)i2c_master_transmit_receive(s_dbg_dev,
                          &idreg, 1, s_dbg_id, sizeof(s_dbg_id), 50);

    /* 0xb0 = firmware version (driver expects 5a 5a 5a 5a after fw upload;
     * note the vendor ref also logs 0,0,0,0 here yet still works, so 0 is
     * not by itself a failure signal). */
    uint8_t b0reg = 0xb0;
    s_dbg_b0_err = (int)i2c_master_transmit_receive(s_dbg_dev,
                          &b0reg, 1, s_dbg_b0, sizeof(s_dbg_b0), 50);

    if (have) xSemaphoreGive(gsl3680_i2c_mutex);

    /* 0xf0 was previously probed with a write+read-back round-trip. That WRITE
     * is the RAM page selector the espcontrol driver relies on; writing it from
     * the HTTP handler while the driver is mid-touch corrupted the page and
     * helped wedge the board. /state must be strictly read-only. Set the f0
     * readback fields to a sentinel (0xEE) so /state no longer touches 0xf0. */
    s_dbg_f0w[0] = s_dbg_f0w[1] = s_dbg_f0w[2] = s_dbg_f0w[3] = 0xEE;
    s_dbg_f0r[0] = s_dbg_f0r[1] = s_dbg_f0r[2] = s_dbg_f0r[3] = 0xEE;
    s_dbg_f0_err = 0;
}

/* Continuous finger-count monitor: reads 0x80 (byte 0 = Finger_num) every
 * 50 ms and latches the max seen + a running event count. This makes a
 * physical tap provably observable: even if a tap happens between /state
 * polls, the latched max will show it. Runs on the touch task after init. */
#if TOUCH_FINGER_MONITOR
static void touch_finger_monitor_task(void *arg)
{
    (void)arg;
    uint8_t reg = 0x80;
    uint8_t b0[1];
    for (;;) {
        if (s_dbg_dev &&
            i2c_master_transmit_receive(s_dbg_dev, &reg, 1, b0, 1, 30) == ESP_OK) {
            int f = b0[0];
            if (f > s_dbg_max_fingers) s_dbg_max_fingers = f;
            if (f > 0) s_dbg_finger_events++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

static char state_char(eTaskState s)
{
    switch (s) {
    case eRunning:   return 'R';
    case eReady:     return 'r';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

void browser_diag(char *buf, size_t len)
{
    size_t o = 0;
    o += snprintf(buf + o, len - o,
                  "task: %s (events=%d, ui_updates=%d, last_text=%zuB)\n",
                  s_task_state == BSTATE_FETCHING ? "FETCHING" : "idle",
                  s_progress, s_ui_update_count, s_last_text_len);
    o += snprintf(buf + o, len - o, "nav queue: %u pending\n",
                  s_nav_q ? (unsigned)uxQueueMessagesWaiting(s_nav_q) : 0u);
    o += snprintf(buf + o, len - o, "heap int: %u free\npsram: %u free\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    o += snprintf(buf + o, len - o, "page buf: %p (%zuB used)\n",
                  (void *)s_page, s_body_len);
    o += snprintf(buf + o, len - o, "last fetch: code=%d hops=%d (1=no redirect)\n",
                  s_last_code, s_hop_n);
    /* Remote (Option B) pipeline stats — inserted early: the /state buffer is
     * 1 KB and the tail sections are large, so the remote line must land in
     * the first third to be visible. */
    if (o + 200 < len) remote_diag(buf, len, &o);
    if (o + 240 < len) settings_diag(buf, len, &o);
    if (o + 60 < len) {
        o += snprintf(buf + o, len - o, "lvlock: held=%lldms by='%s'\n",
                      (long long)s_lock_held_ms, s_lock_holder);
    }
    o += snprintf(buf + o, len - o, "xfer done: %d\n", s_xfer_done_count);
    /* Measure the DSI video-stream rate: on_refresh_done fires once per video
     * frame the GDMA actually transmits fbs[0]->DSI->panel. Sample over 1s.
     * ~50-60/s = stream alive (panel SHOULD show new content); 0 = the video
     * stream is frozen (underrun / GDMA stopped) even though fb0 is written. */
    int r0 = s_refresh_done_count;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    int r1 = s_refresh_done_count;
    o += snprintf(buf + o, len - o, "video fps: %d (frames/s, total=%d)\n",
                  r1 - r0, r1);
    o += snprintf(buf + o, len - o, "flush stuck: %d, flush timeout: %d\n",
                  s_flush_stuck_count, s_flush_timeout_count);
    o += snprintf(buf + o, len - o, "touch: %s (inits=%d, presses=%d, clicks=%lu, points=%d, last=(%u,%u))\n",
                  s_touch_present ? (s_touch ? "up" : "init") : "absent",
                  s_touch_inits, s_touch_presses, (unsigned long)s_clicks, s_touch_points,
                  s_touch_last_x, s_touch_last_y);
    /* Raw GSL3680 read (independent of driver) — decisive touch debug. */
    touch_raw_dump();
    o += snprintf(buf + o, len - o,
                  "touchraw: INT=%d rerr=0x%x id=%02x%02x%02x%02x b0=%02x%02x%02x%02x f0wr=%02x%02x%02x%02x f0rd=%02x%02x%02x%02x f0e=%d 0x80=%02x 0x84=%02x 0x85=%02x (n=%d)\n",
                  s_dbg_int_level, s_dbg_read_err,
                  s_dbg_id[0], s_dbg_id[1], s_dbg_id[2], s_dbg_id[3],
                  s_dbg_b0[0], s_dbg_b0[1], s_dbg_b0[2], s_dbg_b0[3],
                  s_dbg_f0w[0], s_dbg_f0w[1], s_dbg_f0w[2], s_dbg_f0w[3],
                  s_dbg_f0r[0], s_dbg_f0r[1], s_dbg_f0r[2], s_dbg_f0r[3],
                  s_dbg_f0_err,
                  s_dbg_raw[0], s_dbg_raw[4], s_dbg_raw[5], s_dbg_raw_calls);
    /* Driver's OWN I2C path (panel-io) forced read — compare against touchraw.
     * If drv_rerr=0 but drv_0x80=00 while the driver's internal fn is stuck >0,
     * the per-frame read is returning fresh data and Finger_num should update;
     * if drv_rerr!=0, the driver's panel-io read is FAILING (stale Finger_num). */
    {
        uint8_t blk[24];
        esp_err_t derr = esp_lcd_gsl3680_diag_read_block(blk);
        o += snprintf(buf + o, len - o,
                      "touchdrv: fn=%u rerr=0x%x 0x80=%02x 0x84=%02x 0x85=%02x\n",
                      (unsigned)esp_lcd_gsl3680_diag_finger_num(), (int)derr,
                      blk[0], blk[4], blk[5]);
    }
    /* Continuous finger-monitor latches (decisive tap test): maxf = highest
     * 0x80 finger-count byte seen since boot, fe = samples where it was >0.
     * A physical tap MUST raise maxf if the sensing engine is alive. */
    o += snprintf(buf + o, len - o, "touchmon: maxf=%d fe=%d (latched 50ms samples)\n",
                  s_dbg_max_fingers, s_dbg_finger_events);
    /* ---- frame buffer pixel readback (the decisive render test) ----
     * fb0 is the single 800x1280 RGB565 buffer the DSI video path scans out.
     * Sample the exact pixels build_ui places the title/URL/body text in, and
     * count non-black pixels in each text region. If the title row has
     * non-black pixels, the page IS in the frame buffer and the bug is purely
     * in the DSI video output; if the title row is all-black, the DMA2D copy
     * never landed where the panel reads. */
    if (s_fb0) {
        uint16_t *fb = (uint16_t *)s_fb0;
        /* DMA2D wrote the PSRAM frame buffer; invalidate our CPU cache lines
         * (do NOT write-back — that would clobber the DMA2D data) so the read
         * below sees the pixels the panel actually scans out. */
        esp_cache_msync((void *)s_fb0, 800u * 1280u * 2u, ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        o += snprintf(buf + o, len - o, "fb: @%p\n", (void *)s_fb0);
        o += snprintf(buf + o, len - o, "px title(40,40)=#%04x url(40,110)=#%04x body(60,180)=#%04x center(400,640)=#%04x\n",
                      fb[40 * 800 + 40], fb[110 * 800 + 40], fb[180 * 800 + 60], fb[640 * 800 + 400]);
        /* non-black pixel counts per region (width x height) */
        int body_nonblack = 0;
        for (int y = 160; y < 380; y += 2)
            for (int x = 30; x < 790; x += 4)
                if (fb[y * 800 + x]) body_nonblack++;
        int title_nonblack = 0;
        for (int y = 20; y < 80; y += 1)
            for (int x = 20; x < 790; x += 2)
                if (fb[y * 800 + x]) title_nonblack++;
        o += snprintf(buf + o, len - o, "fb nonblack: title(20-80)=%d body(160-380)=%d\n",
                      title_nonblack, body_nonblack);
    } else {
        o += snprintf(buf + o, len - o, "fb: (unavailable)\n");
    }
    static const char *const tn[] = { "browser", "lvgl", "httpd", "c6wifi", "wifi" };
    for (size_t i = 0; i < sizeof(tn) / sizeof(tn[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tn[i]);
        if (!h) continue;
        o += snprintf(buf + o, len - o, "t %-8s: %c hwm=%u\n", tn[i],
                      state_char(eTaskGetState(h)),
                      (unsigned)uxTaskGetStackHighWaterMark(h));
    }
}

/* Decisive video-path test: paint the whole framebuffer a known 3-band
 * pattern via the CPU, then do a FULL cache write-back + invalidate so the
 * GDMA (which reads physical PSRAM, bypassing the CPU cache) is GUARANTEED to
 * see it on the next vsync. If the panel turns red/green/blue, the
 * GDMA->DSI-bridge->panel video path is ALIVE and the bug is that DMA2D's page
 * data specifically isn't reaching GDMA-visible memory (a coherency issue in
 * the render path). If the panel stays on the boot screen, the DSI
 * bridge->panel output stage is dead and no software fix exists. */
esp_err_t browser_fill_test(void)
{
    if (!s_fb0) return ESP_ERR_INVALID_STATE;
    uint16_t *fb = (uint16_t *)s_fb0;
    const uint16_t RED   = 0xF800;  /* R565 full red   */
    const uint16_t GREEN = 0x07E0;  /* R565 full green */
    const uint16_t BLUE  = 0x001F;  /* R565 full blue  */
    for (int y = 0; y < 426; y++)        for (int x = 0; x < 800; x++) fb[y * 800 + x] = RED;
    for (int y = 426; y < 853; y++)      for (int x = 0; x < 800; x++) fb[y * 800 + x] = GREEN;
    for (int y = 853; y < 1280; y++)     for (int x = 0; x < 800; x++) fb[y * 800 + x] = BLUE;
    /* write back to physical PSRAM (DIR_C2M) AND invalidate (clean slate) so
     * the GDMA read is unambiguous. UNALIGNED because fb may not be 4-byte
     * aligned for the msync window. */
    esp_cache_msync(s_fb0, 800u * 1280u * 2u,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ESP_LOGI(BTAG, "fill test: painted 3 bands (R/G/B) + full cache sync, fb @ %p", (void *)s_fb0);
    return ESP_OK;
}
