/*
 * p4browser v2 — Guition JC8012P4A1: MIPI-DSI display + C6 WiFi + captive portal.
 *
 * v1 (proven, preserved verbatim): the display bring-up. This board is a CUSTOM
 *   MIPI-DSI panel driven by the generic esp_lcd_new_panel_dpi() host + a hand
 *   DCS init sequence (espcontrol BASE JC8012P4A1, v1.0 silicon). Display init,
 *   backlight, DSI-PHY LDO and the R->G->B->W cycler below are UNCHANGED from v1.9
 *   so the camera feedback channel stays live no matter what WiFi does.
 *
 * v2 adds the ESP32-C6 co-processor (WiFi) over SDIO, via esp_hosted 2.12.12:
 *   - P4 SDIO slot 1 is IOMUXed to exactly this board's C6 wiring
 *     (CMD19 / CLK18 / D0-3 = 14-17 / reset 54, active-high, 40 MHz). Configured
 *     with the esp-hosted-mcu "FUNC_BOARD" P4 profile + SDIO_SLOT_1 (default).
 *   - The C6 holds its own hosted firmware in ITS flash (the P4 partition table
 *     stays P4-only). On boot we mirror espcontrol's c6_recovery: connect to the
 *     C6, read its fw version, and if it is below the embedded 2.12.12 image we
 *     stream that image over SDIO (esp_hosted_slave_ota_*). The board ships with a
 *     hosted C6, so the connect succeeds and OTA is a safety net, not a first-
 *     boot flasher.
 *   - WiFi: SoftAP "p4browser" (WPA2, 192.168.4.1, auto-DHCP) + an esp_http_server
 *     catch-all captive portal. IDF 5.5's lwIP has no DNS catch-all, so the portal
 *     is enforced over HTTP: any non-/config path serves the portal page. Submitting
 *     SSID+password stores them in NVS and reboots; next boot connects as STA.
 */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_app_desc.h"

/* --- C6 WiFi (esp_hosted 2.12.12 over SDIO) + captive portal --- */
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "driver/ledc.h"
#include "settings.h"

/* --- v3: LVGL browser UI --- */
#include "lvgl.h"

static const char *TAG = "p4v2";

/* browser.c (v3) */
extern esp_err_t browser_init(void);
extern void      browser_navigate(const char *url);
extern void      browser_ui_ip(const char *ip);
extern void      browser_ui_update(const char *url, const char *title, const char *text, int err);
extern esp_err_t browser_display_init(esp_lcd_panel_handle_t panel);
extern esp_err_t browser_fill_test(void);
extern void      browser_diag(char *buf, size_t len);
extern void      browser_touch_start(void);
/* remote.c (v4: NAS-rendered Homey via WS + HW JPEG) */
extern void      remote_start(void);

/* --- Board pins (espcontrol JC8012P4A1) --- */
#define BK_GPIO       23
#define RST_GPIO      27
#define LDO_PHY_CHAN  3
#define LDO_PHY_MV    2500

/* --- Panel geometry / DSI (espcontrol BASE JC8012P4A1, v1.0 silicon) --- */
#define PANEL_W       800
#define PANEL_H       1280
#define DSI_LANES     2
#define DSI_MBPS      1000
#define PCLK_MHZ      60

static esp_lcd_dsi_bus_handle_t  s_bus   = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

/* Auto-generated base JC8012P4A1 init sequence (espcontrol, v1.0 silicon).
 * Flat encoding: [cmd, count, args...] ; a count byte of 0xFF marks a delay
 * (the cmd byte is the delay in ms). The base model carries no sleep-out /
 * display-on — ESPHome appends them as the MIPI default, so we append a
 * 120ms delay + SLPOUT(0x11) + DISPLAY_ON(0x29) at the tail. */
#define SEQ_DELAY 0xFF
static const uint8_t s_init_seq[] = {
#include "jc8012_base_seq.inc"
    120, SEQ_DELAY,      /* ensure >=120ms since reset before waking */
    0x11, 1, 0x00,       /* SLPOUT (Sleep Out) */
    0x29, 1, 0x00,       /* DISPON (Display On) */
};

/* ============================ DISPLAY (v1, verbatim) ============================ */
/* v3: LVGL drives the panel; the flush-ready event callback is registered in
 * browser_display_init() (browser.c). The old ISR/semaphore cycler path is gone. */

/* Backlight: LEDC PWM on GPIO23 (v5 — was a plain GPIO on/off). Brightness is
 * set via the settings screen slider or GET /brightness?level=0-100; the
 * level persists across reboots in NVS ("ui"/"bright", default 100). */
#define BK_TIMER    LEDC_TIMER_0
#define BK_CHANNEL  LEDC_CHANNEL_0
/* 2 kHz, NOT 20 kHz: the P4 LEDC's clock sources are {XTAL, PLL_F80M, RC_FAST}
 * (clk_tree_defs.h SOC_LEDC_CLKS, max 80 MHz) and a valid divisor needs
 * 256 <= div <= 0x3FFFF (ledc.c LEDC_IS_DIV_INVALID, 8 fractional bits).
 * 20 kHz @ 13-bit needs a 163.84 MHz source (div < 256 on every P4 clock), so
 * ledc_timer_config() fails and — the return was never checked — the channel
 * was never enabled: GPIO23 never driven, backlight dead, LCD dark.
 * 2 kHz is valid (XTAL 40 MHz -> div 625, PLL_F80M -> 1250) and is still far
 * above the flicker-fusion threshold for a backlight. */
#define BK_FREQ_HZ  2000
#define BK_DUTY_MAX 8191u   /* 13-bit max duty */
static int s_bright_pct = 100;
static int s_bk_cfg_err = 0;   /* 0 = LEDC ok; <0 = config failed (GPIO fallback) */

static void backlight_apply(void)
{
    if (s_bk_cfg_err != 0) return;   /* GPIO fallback owns the pin */
    uint32_t duty = (uint32_t)(((uint64_t)s_bright_pct * BK_DUTY_MAX) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BK_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BK_CHANNEL);
}

int backlight_get(void)
{
    return s_bright_pct;
}

/* LEDC config result for /state (0 = ok, <0 = ESP error code; fallback on). */
int backlight_cfg_err(void)
{
    return s_bk_cfg_err;
}

void backlight_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == s_bright_pct) return;
    s_bright_pct = pct;
    backlight_apply();
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", (uint8_t)pct);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "backlight set to %d%%", pct);
}

static void init_backlight(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num       = BK_TIMER,
        .freq_hz         = BK_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e1 = ledc_timer_config(&t);
    esp_err_t e2 = ESP_OK;
    if (e1 == ESP_OK) {
        ledc_channel_config_t c = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = BK_CHANNEL,
            .timer_sel  = BK_TIMER,
            .gpio_num   = BK_GPIO,
            .duty       = BK_DUTY_MAX,  /* full until the NVS level is restored */
            .hpoint     = 0,
        };
        e2 = ledc_channel_config(&c);
    }
    if (e1 != ESP_OK || e2 != ESP_OK) {
        /* LEDC can't express the requested freq/resolution on this chip's
         * clocks: fall back to a plain GPIO drive at 100% so the screen can
         * never end up dark for a config error. */
        s_bk_cfg_err = (int)(e1 != ESP_OK ? e1 : e2);
        ESP_LOGE(TAG, "LEDC backlight config FAILED (timer=%d, ch=%d) — GPIO fallback, full brightness",
                 s_bk_cfg_err);
        gpio_config_t g = {
            .pin_bit_mask = 1ULL << BK_GPIO,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&g) == ESP_OK) gpio_set_level(BK_GPIO, 1);
    }
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t b = 100;
        if (nvs_get_u8(h, "bright", &b) == ESP_OK) s_bright_pct = b;
        nvs_close(h);
    }
    if (s_bk_cfg_err == 0) backlight_apply();
    ESP_LOGI(TAG, "backlight ON (GPIO%d, %d Hz PWM, %d%%, ledc err=%d)", BK_GPIO, BK_FREQ_HZ, s_bright_pct, s_bk_cfg_err);
}

static void power_dsi_phy(void)
{
    esp_ldo_channel_config_t ldo = { .chan_id = LDO_PHY_CHAN, .voltage_mv = LDO_PHY_MV };
    esp_ldo_channel_handle_t h;
    if (esp_ldo_acquire_channel(&ldo, &h) == ESP_OK) {
        ESP_LOGI(TAG, "DSI PHY powered (LDO ch%d @%d mV)", LDO_PHY_CHAN, LDO_PHY_MV);
    } else {
        ESP_LOGE(TAG, "DSI PHY LDO acquire FAILED");
    }
}

static esp_err_t init_lcd(void)
{
    /* DSI bus — V1.0 silicon: leave phy_clk_src = 0 so IDF auto-selects the
     * legacy PLL_F20M reference clock (the CORRECT source for pre-v3 P4). */
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = DSI_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus), TAG, "new_dsi_bus");

    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_bus, &dbi, &s_io), TAG, "new_io_dbi");

    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = 0,
        .dpi_clock_freq_mhz = PCLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = PANEL_W, .v_size = PANEL_H,
            .hsync_pulse_width = 20, .hsync_back_porch = 20, .hsync_front_porch = 40,
            .vsync_pulse_width = 4, .vsync_back_porch = 8, .vsync_front_porch = 20,
        },
        /* use_dma2d = false: the CPU-copy path in draw_bitmap does
         * esp_cache_msync(frame_buffer, DIR_C2M) on the DESTINATION after the
         * copy, which updates the L2 so the GDMA video stream reads fresh data.
         * The DMA2D path only syncs the SOURCE draw-buffer and never the dest,
         * leaving the L2 stale -> the panel keeps showing the previous frame
         * (empirically confirmed: /fill CPU write + dest sync updates the panel,
         * DMA2D page writes do not). */
        .flags = { .use_dma2d = false },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_bus, &dpi, &s_panel), TAG, "new_panel_dpi");

    /* HW reset pulse on GPIO27 (espcontrol: true 5ms -> false 5ms -> true). */
    gpio_config_t r = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << RST_GPIO };
    ESP_RETURN_ON_ERROR(gpio_config(&r), TAG, "rst gpio");
    ESP_LOGI(TAG, "panel reset pulse (GPIO%d)", RST_GPIO);
    gpio_set_level(RST_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(RST_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(RST_GPIO, 1);
    int64_t reset_ms = esp_timer_get_time() / 1000;

    esp_err_t e = esp_lcd_panel_init(s_panel);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "panel_init returned %s (continuing — init seq is authoritative)", esp_err_to_name(e));
    }

    /* Walk the board init sequence (espcontrol MipiDsi::setup tail). */
    size_t i = 0;
    int64_t ready_at = reset_ms + 120; /* SLPOUT must be >=120ms after reset */
    while (i + 2 <= sizeof(s_init_seq)) {
        uint8_t cmd = s_init_seq[i++];
        uint8_t x   = s_init_seq[i++];
        if (x == SEQ_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(cmd));
            continue;
        }
        uint8_t n = x & 0x7F;
        if (i + n > sizeof(s_init_seq)) {
            return ESP_ERR_INVALID_ARG; /* malformed */
        }
        if (cmd == 0x11) { /* SLPOUT — ensure we're ready */
            int64_t wait = ready_at - esp_timer_get_time() / 1000;
            if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, cmd, s_init_seq + i, n),
                            TAG, "tx 0x%02X", cmd);
        i += n;
        if (cmd == 0x11) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "init sequence sent (%zu bytes)", sizeof(s_init_seq));

    /* v3: the LVGL flush-ready event callback is registered inside
     * browser_display_init() (browser.c) once the lv_display_t exists, using
     * the display as user_ctx — matches the official espressif DSI+LVGL example. */

    esp_err_t on = esp_lcd_panel_disp_on_off(s_panel, true);
    if (on != ESP_OK) ESP_LOGW(TAG, "disp_on_off: %s (seq already sent 0x29)", esp_err_to_name(on));
    return ESP_OK;
}

/* One-shot full-frame RGB565 fill (used by the cycler and the WiFi status page). */
static esp_err_t fill_rgb565(uint16_t v)
{
    static uint16_t *fb = NULL;
    if (!fb) {
        size_t bytes = (size_t)PANEL_W * PANEL_H * sizeof(uint16_t);
        fb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!fb) { ESP_LOGE(TAG, "fb alloc failed"); return ESP_ERR_NO_MEM; }
    }
    for (int i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = v;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, PANEL_H, fb);
}

/* ============================ C6 WI-FI + PORTAL (v2) ============================ */

/* Embedded C6 hosted firmware (esp-hosted-mcu v2.12.12, sha256
 * bad97ce81e7fcf5f3365898f633b80941ae863db9c754d87a78f89c8f61f2e94) — same
 * version espcontrol pins for this board. EMBED_FILES emits objcopy-style
 * symbols: base `c6_fw_2_12_12_bin` + integer `c6_fw_2_12_12_bin_length`. */
extern const unsigned char c6_fw_2_12_12_bin[];
extern const unsigned int  c6_fw_2_12_12_bin_length;
#define C6_FW_MAJOR 2
#define C6_FW_MINOR 12
#define C6_FW_PATCH 12

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

/* consecutive no-IP disconnects — drives the C6 wedge-recovery escalation */
static int s_sta_disc_streak;
static void s_sta_healthy(void) { s_sta_disc_streak = 0; }

/* The C6 co-processor wedges intermittently (scan fails with 12290, or the
 * link drops and won't come back). A plain esp_wifi_connect can't revive a
 * wedged C6 — only a full esp_hosted deinit/init can. The watchdog lives in
 * c6_wifi_task; the event-loop disconnect handler only RAISES s_c6_reinit_req
 * (it must never call esp_hosted_* from the event-loop context). */
static SemaphoreHandle_t s_c6_mutex = NULL;
static volatile bool s_c6_reinit_req = false;
static bool s_portal_started = false;

/* Full C6 + WiFi stack re-init. Call only from c6_wifi_task. Re-flashes the
 * hosted link and re-inits the (proxied) WiFi driver so a wedged C6 self-heals.
 * The STA netif + event handlers survive; bringup_sta() reapplies the config
 * and reconnects afterwards. */
static esp_err_t c6_hosted_reinit(void)
{
    esp_wifi_stop();
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (esp_hosted_init() != ESP_OK) { ESP_LOGE(TAG, "reinit: esp_hosted_init FAILED"); return ESP_FAIL; }
    if (esp_hosted_connect_to_slave() != ESP_OK) { ESP_LOGE(TAG, "reinit: connect_to_slave FAILED"); return ESP_FAIL; }
    vTaskDelay(pdMS_TO_TICKS(1000));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "reinit: esp_wifi_init FAILED"); return ESP_FAIL; }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) { ESP_LOGE(TAG, "reinit: set_mode FAILED"); return ESP_FAIL; }
    ESP_LOGI(TAG, "C6 re-init complete — re-driving STA");
    return ESP_OK;
}

/* remote.c polls this while waiting for the C6 STA netif to get an IP
 * (esp_netif_get_default_netif() is NULL in esp_hosted builds; the netif is
 * created asynchronously in bringup_sta()). */
esp_netif_t *app_sta_netif(void)
{
    return s_sta_netif;
}
static EventGroupHandle_t s_wifi_eg;
#define BIT_AP_START   (1 << 0)
#define BIT_STA_START  (1 << 1)
#define BIT_GOT_IP     (1 << 2)
#define BIT_STA_DISC   (1 << 3)
#define BIT_AP_STACON  (1 << 4)

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started");
            xEventGroupSetBits(s_wifi_eg, BIT_AP_START);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            xEventGroupSetBits(s_wifi_eg, BIT_AP_STACON);
            break;
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            xEventGroupSetBits(s_wifi_eg, BIT_STA_START);
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *c = data;
            ESP_LOGI(TAG, "STA connected: %.*s", (int)c->ssid_len, c->ssid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "STA disconnected: reason=%u (1=AUTH_EXPIRE 2=AUTH_LEAVING 3=DEAUTH 8=4WAY_HANDSHAKE_FAIL 20=NO_AP_FOUND 21=ASSOC_FAIL 22=ASSOC_RESP_TIMEOUT 23=NOT_AUTHED 26=4WAY_HANDSHAKE_FAIL 28=802_1X_AUTH_FAIL)", d->reason);
            xEventGroupSetBits(s_wifi_eg, BIT_STA_DISC);
            /* The C6 drops association after getting an IP (reason 203/211)
             * and sometimes wedges entirely. A plain esp_wifi_connect cannot
             * revive a wedged C6 — only a full esp_hosted re-init can. We are
             * in event-loop context here, so we only RAISE the flag; the
             * c6_wifi_task watchdog performs the actual re-init + reconnect.
             * (If the c6_wifi_task watchdog is running it will also pick this
             * up on its own next cycle, so this is belt-and-braces.) */
            if (s_sta_netif) {
                esp_netif_ip_info_t ii;
                if (esp_netif_get_ip_info(s_sta_netif, &ii) != ESP_OK || ii.ip.addr == 0) {
                    ESP_LOGW(TAG, "STA no IP after disconnect — requesting C6 re-init (watchdog will handle)");
                    s_c6_reinit_req = true;
                    s_sta_disc_streak++;
                }
            }
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = data;
            char ip[16] = {0};
            esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
            ESP_LOGI(TAG, "STA IP: %s", ip);
            xEventGroupSetBits(s_wifi_eg, BIT_GOT_IP);
            /* healthy link again — clear the wedge counter */
            s_sta_healthy();
        }
    }
}

/* --- Captive portal (esp_http_server catch-all) --- */
static const char *PORTAL_HTML =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>p4browser - configure Wi-Fi</title></head>"
    "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;color:#222\">"
    "<h1>p4browser</h1>"
    "<p>This device is not connected to the internet yet. Enter your Wi-Fi "
    "network to configure it.</p>"
    "<form method=\"post\" action=\"/config\">"
    "<label>Network (SSID)<br><input name=\"ssid\" required style=\"width:100%\"></label><br>"
    "<label>Password<br><input name=\"pass\" style=\"width:100%\"></label><br>"
    "<br><button type=\"submit\" style=\"padding:10px 20px\">Save &amp; connect</button>"
    "</form>"
    "<p style=\"color:#888;font-size:12px\">The device reboots after saving, then "
    "connects to your network.</p>"
    "</body></html>";

static esp_err_t portal_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_health(httpd_req_t *req)
{
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

/* URL-decode a form field in-place: %XX hex and '+' -> space. Browsers
 * percent-encode form values (spaces, &, @, etc.); storing them raw would
 * break WPA passwords containing such characters. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *r = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *r++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else {
            *r++ = (*p == '+') ? ' ' : *p;
        }
    }
    *r = 0;
}

static esp_err_t portal_config(httpd_req_t *req)
{
    char buf[512];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_FAIL; }
    buf[n] = 0;

    char ssid[64] = {0}, pass[64] = {0};
    char *s = strstr(buf, "ssid=");
    if (s) { s += 5; while (*s && *s != '&' && *s != '\r' && *s != '\n') { ssid[strnlen(ssid, 63)] = *s; s++; } }
    char *p = strstr(buf, "pass=");
    if (p) { p += 5; while (*p && *p != '&' && *p != '\r' && *p != '\n') { pass[strnlen(pass, 63)] = *p; p++; } }
    url_decode(ssid);
    url_decode(pass);
    if (ssid[0]) {
        nvs_handle_t h;
        nvs_open("wifi", NVS_READWRITE, &h);
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi config saved: ssid='%s' (pass len=%u)", ssid, (unsigned)strlen(pass));
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h1>Saved</h1><p>Rebooting to connect&hellip;</p>", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    return ESP_OK;
}

/* --- v3: navigation endpoint (phone is the keyboard) ---
 * GET /url?u=https://example.com  -> queue a fetch. The body of the response
 * is a tiny redirect back to "/" so a plain phone tap still works. */
static esp_err_t url_navigate(httpd_req_t *req)
{
    char q[560] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || q[0] == '\0') {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "usage: /url?u=https://example.com\n", HTTPD_RESP_USE_STRLEN);
    }
    /* query is raw (still percent-encoded). Find "u=" — the key itself is not
     * encoded — and take the rest as the (encoded) target URL. */
    char *e = strstr(q, "u=");
    if (!e) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "usage: /url?u=https://example.com\n", HTTPD_RESP_USE_STRLEN);
    }
    e += 2;
    char u[512] = {0};
    strlcpy(u, e, sizeof(u));
    url_decode(u);
    if (u[0] == '\0') {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "usage: /url?u=https://example.com\n", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI(TAG, "NAV requested: %s", u);
    browser_navigate(u);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req,
        "navigation requested — look at the board's display.\n"
        "(this phone page won't update; the board renders the page itself)\n",
        HTTPD_RESP_USE_STRLEN);
}

/* diagnostics: task state + queue + heap, for debugging via phone */
static esp_err_t state_handler(httpd_req_t *req)
{
    /* STATIC: the httpd task stack is small (~4 KB); a 3.2 KB stack buffer
     * here overflowed it and panicked the board on the first request. Static
     * (heap) so the size is safe — 2 KB carries the 16 s fhist hex line. */
    static char buf[2048];
    buf[0] = 0;
    browser_diag(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, strlen(buf));
}

/* /fill — paint the framebuffer a 3-band R/G/B test pattern (full cache sync).
 * Decisive test of whether the GDMA->DSI->panel video path is alive. */
static esp_err_t fill_handler(httpd_req_t *req)
{
    esp_err_t r = browser_fill_test();
    char msg[48];
    int n = snprintf(msg, sizeof(msg), "fill_test: %s\n", esp_err_to_name(r));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, n);
}

/* v5: brightness API — GET /brightness?level=0-100 (no level = query). */
static esp_err_t brightness_handler(httpd_req_t *req)
{
    char q[64] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && q[0]) {
        char *e = strstr(q, "level=");
        if (e) {
            int lvl = atoi(e + 6);
            if (lvl >= 0 && lvl <= 100) {
                backlight_set(lvl);
                char m[48];
                int n = snprintf(m, sizeof(m), "brightness: %d%%\n", backlight_get());
                httpd_resp_set_type(req, "text/plain");
                return httpd_resp_send(req, m, n);
            }
        }
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "usage: /brightness?level=0-100\n", HTTPD_RESP_USE_STRLEN);
    }
    char m[48];
    int n = snprintf(m, sizeof(m), "brightness: %d%%\n", backlight_get());
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, m, n);
}

/* v5: open/close the on-device settings screen — GET /screen?cfg=1 | cfg=0.
 * The LVGL build (incl. the keyboard) runs in its own task: the httpd task
 * stack is too small to build an LVGL screen. */
static void screen_worker(void *arg)
{
    if ((int)(uintptr_t)arg) settings_open(); else settings_close();
    vTaskDelete(NULL);
}
static esp_err_t screen_handler(httpd_req_t *req)
{
    char q[64] = {0};
    int open = 1;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char *e = strstr(q, "cfg=");
        if (e) open = (atoi(e + 4) != 0);
    }
    xTaskCreate(screen_worker, "cfg", 12288, (void *)(uintptr_t)open, 5, NULL);
    char m[48];
    int n = snprintf(m, sizeof(m), "settings: %s\n", open ? "open" : "closed");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, m, n);
}

/* v5: GET /reboot — immediate restart. */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "rebooting\n", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* v5: GET /factory — wipe wifi + renderer config, reboot into the WiFi portal. */
static esp_err_t factory_handler(httpd_req_t *req)
{
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
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "factory reset - rebooting into WiFi portal\n", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_portal(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.stack_size       = 8192;   /* /state + handlers use larger frames */
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }
    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.uri = "/";        u.method = HTTP_GET; u.handler = portal_root;   httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/health";  u.method = HTTP_GET; u.handler = portal_health; httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/config";  u.method = HTTP_POST; u.handler = portal_config; httpd_register_uri_handler(srv, &u);
    /* v3: navigation endpoint — works both in portal-AP and LAN-STA modes */
    memset(&u, 0, sizeof(u));
    u.uri = "/url";     u.method = HTTP_GET; u.handler = url_navigate;  httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/state";   u.method = HTTP_GET; u.handler = state_handler; httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/fill";    u.method = HTTP_GET; u.handler = fill_handler;  httpd_register_uri_handler(srv, &u);
    /* v5: on-device settings + backlight + lifecycle endpoints */
    memset(&u, 0, sizeof(u));
    u.uri = "/brightness"; u.method = HTTP_GET; u.handler = brightness_handler; httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/screen";     u.method = HTTP_GET; u.handler = screen_handler;     httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/reboot";     u.method = HTTP_GET; u.handler = reboot_handler;     httpd_register_uri_handler(srv, &u);
    memset(&u, 0, sizeof(u));
    u.uri = "/factory";    u.method = HTTP_GET; u.handler = factory_handler;    httpd_register_uri_handler(srv, &u);
    ESP_LOGI(TAG, "http server listening on :80 (portal + /url + /state + /fill + /brightness + /screen + /reboot + /factory)");
    return ESP_OK;
}

/* --- SoftAP --- */
static esp_err_t bringup_softap(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_FAIL;
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, "p4browser", sizeof(ap.ap.ssid));
    ap.ap.ssid_len        = 9;
    ap.ap.channel         = 1;
    ap.ap.max_connection  = 4;
    ap.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap.ap.password, "p4browser1", sizeof(ap.ap.password));

    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) return ESP_FAIL;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return ESP_FAIL;
    if (esp_wifi_start() != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "SoftAP starting (ssid 'p4browser')...");
    uint32_t bits = xEventGroupWaitBits(s_wifi_eg, BIT_AP_START, true, false, pdMS_TO_TICKS(8000));
    if (!(bits & BIT_AP_START)) {
        ESP_LOGW(TAG, "AP start event timeout (continuing)");
    }
    start_portal();
    return ESP_OK;
}

/* --- STA (after config saved). Cycles auth modes across attempts:
 * WPA2-PSK -> WPA2/WPA3 transition -> WPA3-SAE. reason=15 (4-way handshake
 * timeout) is the classic signature of a WPA3-SAE AP hit with WPA2-PSK. */
static const wifi_auth_mode_t s_auth_cycle[] =
    { WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_PSK };

/* Diagnostic: active-scan all 2.4G channels and dump every AP the C6 sees.
 * Run once (attempt 0) so the console tells us whether the target SSID is
 * present, on what channel, and with what security — the key to distinguishing
 * "AP not visible / wrong band" from "wrong key". */
static void sta_scan_dump(const char *target)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = WIFI_ACTIVE_SCAN_MIN_DEFAULT_TIME;
    sc.scan_time.active.max = WIFI_ACTIVE_SCAN_MAX_DEFAULT_TIME;
    sc.scan_time.passive    = WIFI_PASSIVE_SCAN_DEFAULT_TIME;
    sc.home_chan_dwell_time = WIFI_SCAN_HOME_CHANNEL_DWELL_DEFAULT_TIME;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) { ESP_LOGW(TAG, "scan_start err=%d", e); return; }
    static wifi_ap_record_t *recs = NULL;
    const uint16_t max = 16;
    uint16_t num = max;
    recs = (wifi_ap_record_t *)heap_caps_malloc(max * sizeof(wifi_ap_record_t), MALLOC_CAP_8BIT);
    if (!recs) { ESP_LOGE(TAG, "scan: malloc fail"); return; }
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) { ESP_LOGW(TAG, "scan_get_ap_records fail"); heap_caps_free(recs); return; }
    ESP_LOGI(TAG, "SCAN: %u APs visible (target '%s')", num, target);
    for (uint16_t i = 0; i < num; i++) {
        const wifi_ap_record_t *ap = &recs[i];
        ESP_LOGI(TAG, "  [%02u] ch=%u rssi=%d auth=%u ssid='%s'",
                 i, ap->primary, (int)ap->rssi, (int)ap->authmode,
                 (const char *)ap->ssid);
    }
    heap_caps_free(recs);
    recs = NULL;
}

static esp_err_t bringup_sta(int attempt)
{
    nvs_handle_t h;
    char ssid[64] = {0}, pass[64] = {0};
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    size_t len = sizeof(ssid); nvs_get_str(h, "ssid", ssid, &len);
    len = sizeof(pass);          nvs_get_str(h, "pass", pass, &len);
    nvs_close(h);
    if (!ssid[0]) return ESP_ERR_NOT_FOUND;
    if (attempt == 0) sta_scan_dump(ssid);

    wifi_auth_mode_t am = s_auth_cycle[attempt % 3];
    ESP_LOGI(TAG, "STA attempt %d/3 to '%s' auth=%d (1=WPA2 5=WPA2+WPA3 7=WPA3)",
             attempt + 1, ssid, (int)am);

    esp_wifi_stop();
    /* Netif + wifi driver are already up (initialised once in c6_wifi_task).
     * Just make sure the STA netif exists and switch the mode. */
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_FAIL;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return ESP_FAIL;

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = am; /* scan threshold = the mode this attempt targets */
    /* WPA3-SAE mandates PMF; declare capability in the sta config. */
    if (am >= WIFI_AUTH_WPA2_WPA3_PSK) {
        sta.sta.pmf_cfg.capable = 1;
        sta.sta.pmf_cfg.required = 0;
    }

    esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (esp_wifi_start() != ESP_OK) return ESP_FAIL;
    xEventGroupClearBits(s_wifi_eg, BIT_STA_START | BIT_GOT_IP | BIT_STA_DISC);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    uint32_t bits = xEventGroupWaitBits(s_wifi_eg, BIT_GOT_IP | BIT_STA_DISC,
                                        false, false, pdMS_TO_TICKS(30000));
    if (bits & BIT_GOT_IP) return ESP_OK;
    return ESP_ERR_TIMEOUT; /* disconnect or timeout — caller retries */
}

/* --- C6 bring-up + optional OTA recovery (espcontrol c6_recovery mirror) --- */
static bool c6_needs_ota(uint32_t maj, uint32_t min, uint32_t pat)
{
    if (maj < C6_FW_MAJOR) return true;
    if (maj > C6_FW_MAJOR) return false;
    if (min < C6_FW_MINOR) return true;
    if (min > C6_FW_MINOR) return false;
    return pat < C6_FW_PATCH;
}

static esp_err_t c6_ota_recover(void)
{
    const size_t fw_size = c6_fw_2_12_12_bin_length;
    const unsigned char *fw = c6_fw_2_12_12_bin;
    ESP_LOGW(TAG, "streaming C6 recovery firmware (%zu bytes) over SDIO...", fw_size);

    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA begin failed");
        return ESP_FAIL;
    }
    const uint8_t chunk[1500];
    size_t off = 0;
    while (off < fw_size) {
        size_t n = (fw_size - off) > sizeof(chunk) ? sizeof(chunk) : (fw_size - off);
        memcpy(chunk, fw + off, n);
        if (esp_hosted_slave_ota_write((uint8_t *)chunk, (uint32_t)n) != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA write failed at %u/%u", (unsigned)off, (unsigned)fw_size);
            esp_hosted_slave_ota_end();
            return ESP_FAIL;
        }
        off += n;
        if ((off % (100 * 1024)) < 1500) {
            ESP_LOGI(TAG, "C6 OTA %u%%", (unsigned)(off * 100 / fw_size));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (esp_hosted_slave_ota_end() != ESP_OK) { ESP_LOGE(TAG, "C6 OTA end failed"); return ESP_FAIL; }
    if (esp_hosted_slave_ota_activate() != ESP_OK) { ESP_LOGE(TAG, "C6 OTA activate failed"); return ESP_FAIL; }
    ESP_LOGI(TAG, "C6 firmware activated — C6 rebooting, reconnecting...");
    return ESP_OK;
}

/* Whole C6 + WiFi bring-up. Runs in its own task so the display cycler keeps
 * going (camera feedback) even if the C6/WiFi path fails. */
static void c6_wifi_task(void *arg)
{
    ESP_LOGI(TAG, "C6 WiFi task starting (esp_hosted over SDIO, C6 target)");

    if (esp_hosted_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init FAILED — display keeps cycling, no WiFi");
        return;
    }
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGE(TAG, "connect_to_slave FAILED — C6 not reachable over SDIO (blank/wrong fw?). "
                      "Display keeps cycling; recheck C6 power/SDIO.");
        return;
    }

    esp_hosted_coprocessor_fwver_t ver = {0};
    bool have_ver = (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK);
    if (have_ver) {
        ESP_LOGI(TAG, "C6 firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32
                 " (target %" PRIu32 ".%" PRIu32 ".%" PRIu32 ")",
                 ver.major1, ver.minor1, ver.patch1,
                 (uint32_t)C6_FW_MAJOR, (uint32_t)C6_FW_MINOR, (uint32_t)C6_FW_PATCH);
    } else {
        ESP_LOGW(TAG, "could not read C6 fw version — assuming recovery needed");
    }

    if (!have_ver || c6_needs_ota(ver.major1, ver.minor1, ver.patch1)) {
        if (c6_ota_recover() != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA recovery failed — no WiFi; display keeps cycling");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1500)); /* let the C6 come back */
        esp_hosted_deinit();
        esp_hosted_init();
        if (esp_hosted_connect_to_slave() != ESP_OK) {
            ESP_LOGE(TAG, "reconnect after OTA failed");
            return;
        }
        ESP_LOGI(TAG, "reconnected to C6 after OTA");
    } else {
        ESP_LOGI(TAG, "C6 firmware OK — no recovery needed");
    }

    /* WiFi stack (proxied to the C6 over SDIO via esp_wifi_remote). */
    s_wifi_eg = xEventGroupCreate();
    s_c6_mutex = xSemaphoreCreateMutex();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init FAILED (esp_wifi_remote/hosted)");
        return;
    }

    /* If a config was saved, go straight to STA; otherwise bring up the portal AP. */
    nvs_handle_t h;
    char ssid[64] = {0};
    bool have_cfg = false;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0]) have_cfg = true;
        nvs_close(h);
    }

    if (have_cfg) {
        /* WATCHDOG: the C6 wedges intermittently (scan 12290, link drops that
         * don't recover). Drive the STA in a loop for the board's lifetime.
         * On failure, full esp_hosted re-init before the next attempt — this
         * is what actually revives a wedged C6. Only fall back to SoftAP if
         * re-init itself can't bring the hosted link back. */
        for (int cycle = 1; ; cycle++) {
            s_c6_reinit_req = false;
            bool up = false;
            for (int attempt = 1; attempt <= 3; attempt++) {
                ESP_LOGI(TAG, "STA cycle %d attempt %d/3", cycle, attempt);
                if (bringup_sta(attempt - 1) == ESP_OK) { up = true; break; }
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (up) {
                ESP_LOGI(TAG, "STA connected — WiFi UP (cycle %d)", cycle);
                s_sta_disc_streak = 0;
                /* v3: start the LAN HTTP server (portal + /url) so the phone can
                 * drive the browser. Get the STA IP for the UI banner. */
                if (s_sta_netif) {
                    esp_netif_ip_info_t ii = {0};
                    if (esp_netif_get_ip_info(s_sta_netif, &ii) == ESP_OK) {
                        char ip[16] = {0};
                        esp_ip4addr_ntoa(&ii.ip, ip, sizeof(ip));
                        ESP_LOGI(TAG, "LAN IP: %s  (navigate via http://%s/url?u=<site>)", ip, ip);
                        browser_ui_ip(ip);
                    }
                }
                if (!s_portal_started) {
                    if (start_portal() == ESP_OK) s_portal_started = true;
                }
                /* Healthy — park and wait. Re-enter the drive loop when the
                 * link dies: the disconnect handler sets s_c6_reinit_req, or
                 * the 60 s IP watchdog fires if the C6 dies silently. */
                int64_t last_ok_ms = esp_timer_get_time() / 1000;
                bool need_reinit = false;
                for (;;) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    if (s_c6_reinit_req) {
                        ESP_LOGW(TAG, "watchdog: C6 re-init requested (disc handler)");
                        need_reinit = true;
                        break;
                    }
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - last_ok_ms > 60000) {
                        esp_netif_ip_info_t ii = {0};
                        bool have_ip = (s_sta_netif &&
                            esp_netif_get_ip_info(s_sta_netif, &ii) == ESP_OK &&
                            ii.ip.addr != 0);
                        if (have_ip) {
                            last_ok_ms = now_ms; /* IP still there — keep waiting */
                        } else {
                            ESP_LOGW(TAG, "watchdog: no IP for 60 s — C6 presumed wedged, re-driving");
                            need_reinit = true;
                            break;
                        }
                    }
                }
                /* A healthy link that dropped means the C6 may be wedged —
                 * full re-init before re-driving (plain bringup won't revive it). */
                if (need_reinit) {
                    if (s_c6_mutex) xSemaphoreTake(s_c6_mutex, portMAX_DELAY);
                    esp_err_t rr = c6_hosted_reinit();
                    if (s_c6_mutex) xSemaphoreGive(s_c6_mutex);
                    if (rr != ESP_OK) {
                        ESP_LOGE(TAG, "C6 re-init failed — rebooting in 3 s");
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        esp_restart();
                    }
                }
                continue; /* next drive cycle */
            }
            /* 3x STA failed this cycle: full re-init of the C6, then loop. */
            ESP_LOGW(TAG, "STA failed 3x (cycle %d) — full C6 re-init, then retry", cycle);
            if (s_c6_mutex) xSemaphoreTake(s_c6_mutex, portMAX_DELAY);
            esp_err_t rr = c6_hosted_reinit();
            if (s_c6_mutex) xSemaphoreGive(s_c6_mutex);
            if (rr != ESP_OK) {
                /* Hosted link itself is dead — re-init failed. A full board
                 * restart is the cleanest reset of a wedged C6. */
                ESP_LOGE(TAG, "C6 re-init failed — rebooting in 3 s");
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        /* (unreachable — the watchdog loops forever) */
        ESP_LOGW(TAG, "STA watchdog exited unexpectedly — falling back to SoftAP portal");
        if (bringup_softap() == ESP_OK) ESP_LOGI(TAG, "SoftAP portal UP (reconfigure via portal)");
    } else {
        if (bringup_softap() == ESP_OK) {
            ESP_LOGI(TAG, "SoftAP 'p4browser' UP — connect a phone to 192.168.4.1 to configure");
            ESP_LOGI(TAG, "  (or http://192.168.4.1/) and submit your network");
        }
    }

    ESP_LOGI(TAG, "C6 WiFi task done — display cycler continues for camera feedback");
    vTaskDelete(NULL);
}

/* ============================ APP MAIN ============================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== p4browser v2 (JC8012P4A1: DSI display + C6 WiFi + captive portal) ===");
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "app: %s  ver %s  idf %s  psram free %u B",
             d->project_name, d->version, d->idf_ver,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    nvs_flash_init(); /* idempotent; needed for wifi NVS + esp_wifi_remote */

    init_backlight();
    power_dsi_phy();
    vTaskDelay(pdMS_TO_TICKS(10));

    if (init_lcd() != ESP_OK) {
        ESP_LOGE(TAG, "init_lcd FAILED — holding (no crash loop)");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* C6 WiFi + portal in its own task (keeps the display responsive). */
    xTaskCreatePinnedToCore(c6_wifi_task, "c6wifi", 16384, NULL, 5, NULL, 0);

    /* v3: LVGL browser UI on the DSI panel (replaces the v1 color cycler). */
    ESP_LOGI(TAG, "DBG: before browser_display_init");
    if (browser_display_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "browser_display_init FAILED — no LVGL UI");
    }
    ESP_LOGI(TAG, "DBG: before browser_init");
    if (browser_init() != ESP_OK) {
        ESP_LOGE(TAG, "browser_init FAILED — no fetch task");
    }
    /* Touch (GSL3680) init runs in its own low-priority task — the driver
     * uploads firmware to the chip (~800 ms), so keep it off the boot path. */
    ESP_LOGI(TAG, "DBG: before browser_touch_start");
    browser_touch_start();
    ESP_LOGI(TAG, "DBG: before remote_start");
    /* v4: remote-render pipeline (WS to the NAS renderer + HW JPEG into the
     * panel fb). Connects once the C6 STA netif has an IP; owns the display +
     * touch until it disconnects. */
    remote_start();
    ESP_LOGI(TAG, "DBG: after remote_start");
    ESP_LOGI(TAG, "p4browser v4 ready — LVGL UI + remote renderer up");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
