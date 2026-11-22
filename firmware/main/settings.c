/*
 * p4browser settings screen (v5) — on-device configuration over LVGL.
 *
 * Opened by a 3-second long-press on the touchscreen (detected in
 * remote.c's touch task) or GET /screen?cfg=1. Contents:
 *   - Wi-Fi SSID / password (NVS "wifi")  — saving reboots into the new STA
 *   - Renderer host / port / url (NVS "remote") — saving applies LIVE
 *     (remote_stop + remote_start, no reboot)
 *   - Backlight slider (NVS "ui"/"bright") — live, applied as it moves
 *   - Back / Save & apply / Reboot / Factory (two-tap confirm)
 *
 * All LVGL object work happens under browser.c's LVGL lock (same proven
 * pattern as remote_ui_ready). While this screen is up, remote.c stands
 * down: no frame decode/present, no touch read — the LVGL indev becomes the
 * sole touch reader, and the DSI video stream keeps scanning fbs[0] which
 * LVGL now owns again (full-screen invalidate on open/close).
 */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "lvgl.h"
#include "settings.h"

static const char *STAG = "settings";

/* browser.c (LVGL lock + framebuffer + LAN IP) */
extern void  browser_lvgl_lock(void);
extern void  browser_lvgl_unlock(void);
extern bool  browser_lvgl_held(void);
extern bool  browser_lvgl_trylock(uint32_t ms);
extern void  browser_fb_invalidate(void);
extern void  browser_ip_get(char *buf, size_t len);

/* ---- screen state ---- */
static lv_obj_t      *s_scr = NULL;
static lv_obj_t      *s_prev_scr = NULL;
static lv_obj_t      *s_status = NULL;     /* yellow status line under the title */
static lv_obj_t      *s_title  = NULL;     /* the "Settings" title label */
static lv_obj_t      *s_kb = NULL;         /* shared keyboard (shown on focus) */
static lv_obj_t      *s_kb_target = NULL;
static lv_obj_t      *s_ta_ssid = NULL;
static lv_obj_t      *s_ta_pass = NULL;
static lv_obj_t      *s_ta_host = NULL;
static lv_obj_t      *s_ta_port = NULL;
static lv_obj_t      *s_ta_url = NULL;
static lv_obj_t      *s_slider = NULL;
static lv_obj_t      *s_slider_lbl = NULL;
static lv_obj_t      *s_btn_factory = NULL;
static lv_obj_t      *s_btn_factory_lbl = NULL;
static volatile bool  s_active = false;
static volatile bool  s_boot_hint = false;   /* opened by the boot watchdog (no stream) */
static int64_t        s_factory_arm_ms = 0;

static void set_status(const char *t)
{
    if (s_status) lv_label_set_text(s_status, t);
}

/* ============================ field / keyboard wiring ============================ */

static void on_ta_focus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_mode(s_kb,
                         (ta == s_ta_port) ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, ta);   /* replaces any previous target */
    s_kb_target = ta;
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void on_ta_defocus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (s_kb_target != ta) return;
    lv_keyboard_set_textarea(s_kb, NULL);
    s_kb_target = NULL;
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void wire_textarea(lv_obj_t *ta)
{
    lv_obj_add_event_cb(ta, on_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_ta_defocus, LV_EVENT_DEFOCUSED, NULL);
}

/* Brightness slider: live — each notch applies + persists immediately. */
static void on_slider(lv_event_t *e)
{
    (void)e;
    int v = lv_slider_get_value(s_slider);
    char t[24];
    snprintf(t, sizeof(t), "%d %%", v);
    lv_label_set_text(s_slider_lbl, t);
    backlight_set(v);
}

/* ============================ button handlers ============================
 * All of these run inside the LVGL task (event callbacks), which holds the
 * LVGL lock — a vTaskDelay here briefly pauses the UI, which is fine for
 * one-shot reboot/reconnect actions. */

static void on_back(lv_event_t *e)
{
    (void)e;
    settings_close();
}

static void on_reboot(lv_event_t *e)
{
    (void)e;
    set_status("rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void on_factory(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_factory_arm_ms && now - s_factory_arm_ms > 6000) s_factory_arm_ms = 0;
    if (!s_factory_arm_ms) {
        s_factory_arm_ms = now;
        set_status("tap Factory again within 6 s to confirm");
        lv_label_set_text(s_btn_factory_lbl, "CONFIRM?");
        return;
    }
    s_factory_arm_ms = 0;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open("remote", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "host");
        nvs_erase_key(h, "port");
        nvs_erase_key(h, "url");
        nvs_commit(h);
        nvs_close(h);
    }
    set_status("factory reset - rebooting into WiFi portal...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void on_save(lv_event_t *e)
{
    (void)e;
    char ssid[64] = {0}, pass[64] = {0}, host[64] = {0}, port_s[16] = {0}, url[160] = {0};
    strlcpy(ssid,   lv_textarea_get_text(s_ta_ssid), sizeof(ssid));
    strlcpy(pass,   lv_textarea_get_text(s_ta_pass), sizeof(pass));
    strlcpy(host,   lv_textarea_get_text(s_ta_host), sizeof(host));
    strlcpy(port_s, lv_textarea_get_text(s_ta_port), sizeof(port_s));
    strlcpy(url,    lv_textarea_get_text(s_ta_url),  sizeof(url));

    int port = atoi(port_s);
    if (!host[0] || port < 1 || port > 65535) {
        set_status("renderer host/port invalid");
        return;
    }
    if (!url[0]) strlcpy(url, CFG_DEFAULT_URL, sizeof(url));

    /* current effective renderer endpoint (for live-restart decision) */
    char ohost[64] = {0};
    int  oport = 0;
    char ourl[160] = {0};
    remote_cfg_get(ohost, sizeof(ohost), &oport, ourl, sizeof(ourl));

    /* stored wifi (for reboot decision) */
    char osid[64] = {0}, opass[64] = {0};
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(osid);
        if (nvs_get_str(h, "ssid", osid, &l) != ESP_OK) osid[0] = 0;
        l = sizeof(opass);
        if (nvs_get_str(h, "pass", opass, &l) != ESP_OK) opass[0] = 0;
        nvs_close(h);
    }

    /* persist */
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open("remote", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "host", host);
        nvs_set_u16(h, "port", (uint16_t)port);
        nvs_set_str(h, "url", url);
        nvs_commit(h);
        nvs_close(h);
    }

    bool wifi_changed = (strcmp(osid, ssid) != 0) || (strcmp(opass, pass) != 0);
    bool rend_changed = (strcmp(ohost, host) != 0) || (oport != port) || (strcmp(ourl, url) != 0);

    if (wifi_changed) {
        set_status("WiFi saved - rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    if (rend_changed) {
        set_status("renderer updated - reconnecting...");
        vTaskDelay(pdMS_TO_TICKS(300));
        remote_stop();
        remote_start();
    } else {
        set_status("saved");
    }
}

/* Reload the pinned dashboard on the renderer WITHOUT a board reboot: tell the
 * NAS renderer (GET /reload on its control port) to re-fetch the page in the
 * running Chrome. The network call can block up to ~6 s, so it runs in its own
 * task — the LVGL task only sets the "reloading…" label, and the task updates
 * the result under the LVGL lock (so the UI never freezes on a slow renderer). */
static void reload_ui_task(void *arg)
{
    (void)arg;
    bool ok = renderer_reload_do();
    if (browser_lvgl_trylock(3000)) {
        set_status(ok ? "reloaded \xE2\x9C\x93" : "reload failed \xE2\x80\x94 renderer unreachable");
        browser_lvgl_unlock();
    }
    vTaskDelete(NULL);
}

static void on_reload(lv_event_t *e)
{
    (void)e;
    set_status("reloading dashboard…");
    xTaskCreate(reload_ui_task, "rdreload", 8192, NULL, 5, NULL);
}

/* ============================ screen build ============================ */

static void add_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_pad_top(l, 24, 0);
}

static void add_field_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9AA4AD), 0);
    lv_obj_set_style_pad_top(l, 10, 0);
}

static lv_obj_t *add_field(lv_obj_t *parent, int w, int h, const lv_font_t *font, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_style_text_font(ta, font, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x3A4750), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 10, 0);
    lv_textarea_set_one_line(ta, true);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (password) lv_textarea_set_password_mode(ta, true);
    wire_textarea(ta);
    return ta;
}

/* settings: one column of the landscape content box (flex-stacks children) */
static lv_obj_t *settings_mk_col(lv_obj_t *parent, int32_t x, int32_t w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, 464);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, x, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_top(c, 24, 0);
    lv_obj_set_style_pad_bottom(c, 20, 0);
    lv_obj_set_style_pad_left(c, 20, 0);
    lv_obj_set_style_pad_right(c, 20, 0);
    lv_obj_set_layout(c, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return c;
}

static lv_obj_t *add_button(lv_obj_t *parent, int x, const char *text, uint32_t bg, lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 232, 80);
    /* BOTTOM align y-ofs is ADDITIVE in +y (DOWN): lv_area_align sets
     * y1 = (base_h - obj_h) + ofs_y, so a POSITIVE ofs_y pushes the button
     * BELOW the screen edge (the "sunk buttons" bug: +40 then +60 each cut
     * the row off the bottom). Negative lifts it into view; -60 leaves a
     * clean 60 px margin under the button row. */
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -60);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_settings_screen(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 1280, 800);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* title + status */
    lv_obj_t *t = lv_label_create(s_scr);
    s_title = t;
    lv_label_set_text(t, "Settings");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 40, 20);

    s_status = lv_label_create(s_scr);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_width(s_status, 1200);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 40, 76);

    /* content box (landscape: wide + short, above the button row) */
    lv_obj_t *box = lv_obj_create(s_scr);
    lv_obj_set_size(box, 1240, 504);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 20, 136);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);

    /* three columns: Wi-Fi | Renderer | Display */
    lv_obj_t *col_wi = settings_mk_col(box, 0, 375);
    lv_obj_t *col_re = settings_mk_col(box, 405, 400);
    lv_obj_t *col_di = settings_mk_col(box, 825, 375);

    /* Wi-Fi */
    add_section(col_wi, "Wi-Fi");
    add_field_label(col_wi, "SSID");
    s_ta_ssid = add_field(col_wi, 335, 68, &lv_font_montserrat_24, false);
    add_field_label(col_wi, "Password");
    s_ta_pass = add_field(col_wi, 335, 68, &lv_font_montserrat_24, true);

    /* Renderer */
    add_section(col_re, "Renderer (NAS)");
    add_field_label(col_re, "Host");
    s_ta_host = add_field(col_re, 360, 68, &lv_font_montserrat_24, false);
    add_field_label(col_re, "Port");
    s_ta_port = add_field(col_re, 360, 68, &lv_font_montserrat_24, false);
    add_field_label(col_re, "Dashboard URL (loaded by the renderer)");
    s_ta_url = add_field(col_re, 360, 68, &lv_font_montserrat_20, false);

    /* Display */
    add_section(col_di, "Display");
    add_field_label(col_di, "Brightness");
    lv_obj_t *row = lv_obj_create(col_di);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    s_slider = lv_slider_create(row);
    lv_obj_set_size(s_slider, 240, 44);
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    s_slider_lbl = lv_label_create(row);
    lv_label_set_text(s_slider_lbl, "100 %");
    lv_obj_set_style_text_font(s_slider_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_slider_lbl, lv_color_white(), 0);
    lv_obj_set_style_pad_left(s_slider_lbl, 20, 0);

    /* bottom button bar (always visible, below the scroll area). Five buttons
     * at 232 px wide: Back / Save & apply / Reboot / Reload / Factory. */
    add_button(s_scr, 20, "Back", 0x37474F, on_back, NULL);
    add_button(s_scr, 272, "Save & apply", 0x2E7D32, on_save, NULL);
    add_button(s_scr, 524, "Reboot", 0x37474F, on_reboot, NULL);
    add_button(s_scr, 776, "Reload", 0x1565C0, on_reload, NULL);
    s_btn_factory = add_button(s_scr, 1028, "Factory", 0xC62828, on_factory, &s_btn_factory_lbl);

    /* shared keyboard, shown while a field is focused (sits above the bar) */
    s_kb = lv_keyboard_create(s_scr);
    lv_obj_set_size(s_kb, 1200, 440);
    /* -148 (NOT +148): BOTTOM align y-ofs is additive in +y (down), so +148
     * clipped the keyboard's bottom third below the screen edge. -148 lifts
     * it so its bottom sits just above the button bar. */
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_LEFT, 40, -148);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* ============================ public API ============================ */

void settings_open(void)
{
    /* Bounded lock take: this runs in its own task (long-press gesture worker
     * / /screen HTTP worker). A portMAX_DELAY take would wedge that task
     * forever if the LVGL task is spinning (the 2026-11-02 wedge: a settings
     * open froze the board, /state and /reboot included). */
    bool re = browser_lvgl_held();
    if (!re && !browser_lvgl_trylock(4000)) {
        ESP_LOGE(STAG, "LVGL lock busy >4 s — settings open aborted (LVGL wedged?)");
        return;
    }
    if (s_active) {
        if (!re) browser_lvgl_unlock();
        return;
    }
    if (!s_scr) build_settings_screen();
    s_active = true;

    /* populate current values */
    char host[64] = {0}, url[160] = {0}, ip[32] = {0};
    int port = 0;
    remote_cfg_get(host, sizeof(host), &port, url, sizeof(url));
    browser_ip_get(ip, sizeof(ip));
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    lv_textarea_set_text(s_ta_host, host);
    lv_textarea_set_text(s_ta_port, port_s);
    lv_textarea_set_text(s_ta_url, url);
    char ssid[64] = {0}, pass[64] = {0};
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &l) != ESP_OK) ssid[0] = 0;
        l = sizeof(pass);
        if (nvs_get_str(h, "pass", pass, &l) != ESP_OK) pass[0] = 0;
        nvs_close(h);
    }
    lv_textarea_set_text(s_ta_ssid, ssid);
    lv_textarea_set_text(s_ta_pass, pass);
    int b = backlight_get();
    lv_slider_set_value(s_slider, b, LV_ANIM_OFF);
    char bt[24];
    snprintf(bt, sizeof(bt), "%d %%", b);
    lv_label_set_text(s_slider_lbl, bt);

    /* drop every stale video line from the GDMA's L2 before LVGL takes over */
    browser_fb_invalidate();
    s_prev_scr = lv_screen_active();
    lv_screen_load(s_scr);
    char st[160];
    if (s_boot_hint) {
        /* Opened by the boot watchdog because no frame ever arrived. Tell the
         * user exactly what is wrong and what to do, rather than a black page. */
        lv_label_set_text(s_title, "Config \xe2\x80\x94 no video yet");
        if (!ip[0])
            snprintf(st, sizeof(st),
                     "No Wi-Fi. Set SSID+password, tap SAVE. IP: none");
        else
            snprintf(st, sizeof(st),
                     "No stream from %s:%d. Check the renderer/PowerHub, or fix the address. IP: %s",
                     host, port, ip);
    } else {
        lv_label_set_text(s_title, "Settings");
        snprintf(st, sizeof(st), "%s  |  %s:%d", ip[0] ? ip : "no IP", host, port);
    }
    set_status(st);
    if (!re) browser_lvgl_unlock();
    ESP_LOGI(STAG, "settings screen open (ip=%s renderer=%s:%d)%s", ip, host, port,
             s_boot_hint ? " [boot-hint]" : "");
}

/* Boot-hint variant: same screen, but with a "why am I seeing this" status.
 * The boot watchdog calls this when no frame ever arrived. */
void settings_open_boot(void)
{
    if (settings_active()) return;
    s_boot_hint = true;
    settings_open();
    s_boot_hint = false;   /* consumed; a later manual Back/Save clears anyway */
}

void settings_close(void)
{
    s_boot_hint = false;   /* next open is a normal manual open */

    /* Re-entrant lock: the settings Back button fires this from an LVGL event
     * callback — INSIDE lv_timer_handler, i.e. with s_lvgl_lock already held
     * by the lvgl task. The old plain browser_lvgl_lock() there was a
     * guaranteed self-deadlock (FreeRTOS mutexes are not recursive): one tap
     * on Back would freeze the board exactly like a wedge does. */
    bool re = browser_lvgl_held();
    if (!re && !browser_lvgl_trylock(2000)) {
        ESP_LOGE(STAG, "LVGL lock busy >2 s — settings close aborted (LVGL wedged?)");
        return;
    }
    if (!s_active) {
        if (!re) browser_lvgl_unlock();
        return;
    }
    s_active = false;
    if (s_kb_target) {
        lv_keyboard_set_textarea(s_kb, NULL);
        s_kb_target = NULL;
    }
    if (s_prev_scr && lv_obj_is_valid(s_prev_scr)) {
        lv_screen_load(s_prev_scr);
    }
    browser_fb_invalidate();
    /* the long-press finger may still be down — swallow its release so the
     * dashboard doesn't receive a stray click (remote.c). */
    remote_touch_reset();
    if (!re) browser_lvgl_unlock();
    ESP_LOGI(STAG, "settings screen closed");
}

bool settings_active(void)
{
    return s_active;
}

void settings_diag(char *buf, size_t len, size_t *o)
{
    char host[64] = {0}, url[160] = {0}, ip[32] = {0};
    int port = 0;
    remote_cfg_get(host, sizeof(host), &port, url, sizeof(url));
    browser_ip_get(ip, sizeof(ip));
    *o += snprintf(buf + *o, len - *o,
                   "settings: open=%d bright=%d bk=%d ip=%s renderer=%s:%d url=%s\n",
                   (int)s_active, backlight_get(), backlight_cfg_err(), ip[0] ? ip : "-", host, port, url);
}
