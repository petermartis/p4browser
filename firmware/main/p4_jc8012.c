/*
 * p4_jc8012.c — MIPI-DSI panel for the Guition JC8012P4A1 (10.1" 800x1280).
 *
 * This is a CUSTOM DSI panel: it is NOT driven by the Waveshare
 * esp_lcd_jd9365 vendor driver (whose init registers do not match this board).
 * Instead we use the generic esp_lcd_new_panel_dpi() host and send a hand-written
 * DCS init sequence, exactly as the proven espcontrol firmware for this board does.
 *
 * Pinout / timing / init sequence ported from:
 *   github.com/jtenniswood/espcontrol  (devices/guition-esp32-p4-jc8012p4a1-v3)
 */
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "p4_jc8012";

/* --- Proven init sequence (DCS commands) for this exact panel. ---
 * Flat encoding: [cmd, n, d0..dn-1] ... A count byte of 0x80 marks a delay,
 * where the *command* byte is the delay in ms.
 *
 * Decoded (from espcontrol device.yaml init_sequence):
 *   E0 00  E1 93  E2 65  E3 F8        page-select / control
 *   80 01                       (single-param 0x80=1)
 *   E0 01  00 00
 *   01 44  03 10  04 38  0C 74
 *   17 00  18 AF  19 00  1A 00  1B AF  1C 00
 *   35 26  37 09  38 04  39 00  3A 01      (0x3A: pixel format = 16bit RGB565)
 *   3C 78  3D FF  3E FF  3F 7F
 *   40 06  41 A0  42 81  43 1E  44 0D  45 28
 *   55 02  57 69  59 0A  5A 2A  5B 17  5D 7F  5E 6B  5F 5C
 *   60 50  61 4C  62 3E  63 41  64 2B  65 43  66 42  67 43
 *   68 62  69 52  6A 5A  6B 4C  6C 48  6D 3A  6E 28  6F 10
 *   70 7F  71 6B  72 5C  73 50  74 4C  75 3E  76 41  77 2B
 *   78 43  79 42  7A 43  7B 62  7C 52  7D 5A  7E 4C  7F 48
 *   80 3A  81 28  82 10
 *   E0 02  00 42  01 42  02 40  03 40  04 5E  05 5E  06 5F  07 5F
 *   08 5F  09 57  0A 57  0B 77  0C 77  0D 47  0E 47  0F 45
 *   10 45  11 4B  12 4B  13 49  14 49  15 5F  16 41  17 41
 *   18 40  19 40  1A 5E  1B 5E  1C 5F  1D 5F  1E 5F  1F 57
 *   20 57  21 77  22 77  23 46  24 46  25 44  26 44  27 4A
 *   28 4A  29 48  2A 48  2B 5F  2C 01  2D 01  2E 00  2F 00
 *   30 1F  31 1F  32 1E  33 1E  34 1F  35 17  36 17  37 37
 *   38 37  39 08  3A 08  3B 0A  3C 0A  3D 04  3E 04  3F 06
 *   40 06  41 1F  42 02  43 02  44 00  45 00  46 1F  47 1F
 *   48 1E  49 1E  4A 1F  4B 17  4C 17  4D 37  4E 37  4F 09
 *   50 09  51 0B  52 0B  53 05  54 05  55 07  56 07  57 1F
 *   58 40  5B 30  5C 00  5D 34  5E 05  5F 02
 *   63 00  64 6A  67 73  68 07  69 08  6A 6A  6B 08  6C 00
 *   6D 00  6E 00  6F 88
 *   75 FF  77 DD  78 2C  79 15  7A 17  7D 14  7E 82
 *   E0 04  00 0E  02 B3  09 60  0E 48  37 58  2B 0F
 *   E0 05  15 1D
 *   E0 00
 *   E6 02  E7 0C
 *   11 00                       (SLPOUT) -> wait until 120ms-after-reset, then +10ms
 *   DELAY 120ms
 *   29 00                       (DISPLAY ON)
 *   DELAY 20ms
 */
#define DELAY_FLAG 0x80
static const uint8_t jc8012_init_seq[] = {
    0xE0, 0x00,
    0xE1, 0x93,
    0xE2, 0x65,
    0xE3, 0xF8,
    0x80, 0x01,
    0xE0, 0x01, 0x00, 0x00,
    0x01, 0x44,
    0x03, 0x10,
    0x04, 0x38,
    0x0C, 0x74,
    0x17, 0x00,
    0x18, 0xAF,
    0x19, 0x00,
    0x1A, 0x00,
    0x1B, 0xAF,
    0x1C, 0x00,
    0x35, 0x26,
    0x37, 0x09,
    0x38, 0x04,
    0x39, 0x00,
    0x3A, 0x01,
    0x3C, 0x78,
    0x3D, 0xFF,
    0x3E, 0xFF,
    0x3F, 0x7F,
    0x40, 0x06,
    0x41, 0xA0,
    0x42, 0x81,
    0x43, 0x1E,
    0x44, 0x0D,
    0x45, 0x28,
    0x55, 0x02,
    0x57, 0x69,
    0x59, 0x0A,
    0x5A, 0x2A,
    0x5B, 0x17,
    0x5D, 0x7F,
    0x5E, 0x6B,
    0x5F, 0x5C,
    0x60, 0x50,
    0x61, 0x4C,
    0x62, 0x3E,
    0x63, 0x41,
    0x64, 0x2B,
    0x65, 0x43,
    0x66, 0x42,
    0x67, 0x43,
    0x68, 0x62,
    0x69, 0x52,
    0x6A, 0x5A,
    0x6B, 0x4C,
    0x6C, 0x48,
    0x6D, 0x3A,
    0x6E, 0x28,
    0x6F, 0x10,
    0x70, 0x7F,
    0x71, 0x6B,
    0x72, 0x5C,
    0x73, 0x50,
    0x74, 0x4C,
    0x75, 0x3E,
    0x76, 0x41,
    0x77, 0x2B,
    0x78, 0x43,
    0x79, 0x42,
    0x7A, 0x43,
    0x7B, 0x62,
    0x7C, 0x52,
    0x7D, 0x5A,
    0x7E, 0x4C,
    0x7F, 0x48,
    0x80, 0x3A,
    0x81, 0x28,
    0x82, 0x10,
    0xE0, 0x02, 0x00, 0x42, 0x01, 0x42, 0x02, 0x40, 0x03, 0x40, 0x04, 0x5E, 0x05, 0x5E, 0x06, 0x5F, 0x07, 0x5F,
    0x08, 0x5F, 0x09, 0x57, 0x0A, 0x57, 0x0B, 0x77, 0x0C, 0x77, 0x0D, 0x47, 0x0E, 0x47, 0x0F, 0x45,
    0x10, 0x45, 0x11, 0x4B, 0x12, 0x4B, 0x13, 0x49, 0x14, 0x49, 0x15, 0x5F, 0x16, 0x41, 0x17, 0x41,
    0x18, 0x40, 0x19, 0x40, 0x1A, 0x5E, 0x1B, 0x5E, 0x1C, 0x5F, 0x1D, 0x5F, 0x1E, 0x5F, 0x1F, 0x57,
    0x20, 0x57, 0x21, 0x77, 0x22, 0x77, 0x23, 0x46, 0x24, 0x46, 0x25, 0x44, 0x26, 0x44, 0x27, 0x4A,
    0x28, 0x4A, 0x29, 0x48, 0x2A, 0x48, 0x2B, 0x5F, 0x2C, 0x01, 0x2D, 0x01, 0x2E, 0x00, 0x2F, 0x00,
    0x30, 0x1F, 0x31, 0x1F, 0x32, 0x1E, 0x33, 0x1E, 0x34, 0x1F, 0x35, 0x17, 0x36, 0x17, 0x37, 0x37,
    0x38, 0x37, 0x39, 0x08, 0x3A, 0x08, 0x3B, 0x0A, 0x3C, 0x0A, 0x3D, 0x04, 0x3E, 0x04, 0x3F, 0x06,
    0x40, 0x06, 0x41, 0x1F, 0x42, 0x02, 0x43, 0x02, 0x44, 0x00, 0x45, 0x00, 0x46, 0x1F, 0x47, 0x1F,
    0x48, 0x1E, 0x49, 0x1E, 0x4A, 0x1F, 0x4B, 0x17, 0x4C, 0x17, 0x4D, 0x37, 0x4E, 0x37, 0x4F, 0x09,
    0x50, 0x09, 0x51, 0x0B, 0x52, 0x0B, 0x53, 0x05, 0x54, 0x05, 0x55, 0x07, 0x56, 0x07, 0x57, 0x1F,
    0x58, 0x40,
    0x5B, 0x30,
    0x5C, 0x00,
    0x5D, 0x34,
    0x5E, 0x05,
    0x5F, 0x02,
    0x63, 0x00,
    0x64, 0x6A,
    0x67, 0x73,
    0x68, 0x07,
    0x69, 0x08,
    0x6A, 0x6A,
    0x6B, 0x08,
    0x6C, 0x00,
    0x6D, 0x00,
    0x6E, 0x00,
    0x6F, 0x88,
    0x75, 0xFF,
    0x77, 0xDD,
    0x78, 0x2C,
    0x79, 0x15,
    0x7A, 0x17,
    0x7D, 0x14,
    0x7E, 0x82,
    0xE0, 0x04, 0x00, 0x0E, 0x02, 0xB3, 0x09, 0x60, 0x0E, 0x48, 0x37, 0x58, 0x2B, 0x0F,
    0xE0, 0x05, 0x15, 0x1D,
    0xE0, 0x00,
    0xE6, 0x02,
    0xE7, 0x0C,
    0x11, 0x00,                       // SLPOUT
    DELAY_FLAG, 120,                  // delay 120ms (panel ready ~120ms after reset)
    0x29, 0x00,                       // DISPLAY ON
    DELAY_FLAG, 20,                   // delay 20ms
};

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int64_t reset_done_ms;
} jc8012_panel_t;

static esp_err_t jc8012_reset(esp_lcd_panel_t *panel)
{
    jc8012_panel_t *p = __containerof(panel, jc8012_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        // HW reset pulse: high -> low(5ms) -> high
        esp_rom_gpio_pad_conf(p->reset_gpio_num, 0);
        // use gpio driver
    }
    panel->delay_ms(5);
    panel->delay_ms(5);
    p->reset_done_ms = panel->get_time_us() / 1000;
    return ESP_OK;
}

static esp_err_t jc8012_init(esp_lcd_panel_t *panel)
{
    jc8012_panel_t *p = __containerof(panel, jc8012_panel_t, base);
    const uint8_t *s = jc8012_init_seq;
    size_t i = 0;
    while (i < sizeof(jc8012_init_seq)) {
        uint8_t cmd = s[i++];
        uint8_t x = s[i++];
        if (x == DELAY_FLAG) {
            panel->delay_ms(cmd);
            continue;
        }
        uint8_t n = x & 0x7F;
        if (i + n > sizeof(jc8012_init_seq)) {
            ESP_LOGE(TAG, "malformed init seq");
            return ESP_ERR_INVALID_ARG;
        }
        if (cmd == 0x11) {
            // ensure >=120ms since reset before SLPOUT
            int64_t now = panel->get_time_us() / 1000;
            int64_t wait = (p->reset_done_ms + 120) - now;
            if (wait > 0) panel->delay_ms(wait);
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, cmd, s + i, n), TAG, "tx 0x%02X", cmd);
        i += n;
        if (cmd == 0x11) panel->delay_ms(10);
    }
    return ESP_OK;
}

static esp_err_t jc8012_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    jc8012_panel_t *p = __containerof(panel, jc8012_panel_t, base);
    return esp_lcd_panel_io_tx_param(p->io, on ? 0x29 : 0x28, NULL, 0);
}

static esp_err_t jc8012_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    return ESP_OK; // native offset is 0
}

esp_err_t p4_jc8012_panel_create(esp_lcd_dsi_bus_handle_t bus,
                                 const esp_lcd_panel_dev_config_t *dev_config,
                                 esp_lcd_dsi_panel_config_t *dsi_cfg,
                                 esp_lcd_panel_handle_t *ret)
{
    // 1) Build the DSI bus config from the board's known-good values
    dsi_cfg->lane_num = 2;
    dsi_cfg->lane_bit_rate_mbps = 840;
    dsi_cfg->dpi_config.video_timing.h_size = 800;
    dsi_cfg->dpi_config.video_timing.v_size = 1280;
    dsi_cfg->dpi_config.video_timing.hsync_pulse_width = 20;
    dsi_cfg->dpi_config.video_timing.hsync_back_porch = 20;
    dsi_cfg->dpi_config.video_timing.hsync_front_porch = 40;
    dsi_cfg->dpi_config.video_timing.vsync_pulse_width = 4;
    dsi_cfg->dpi_config.video_timing.vsync_back_porch = 10;
    dsi_cfg->dpi_config.video_timing.vsync_front_porch = 30;
    dsi_cfg->dpi_config.dpi_clock_freq_mhz = 70;
    dsi_cfg->dpi_config.in_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dsi_cfg->dpi_config.num_fbs = 1;
    dsi_cfg->dpi_config.flags.use_dma2d = true;
    dsi_cfg->bus_id = 0;

    // 2) Create the DSI bus + DBI io
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = dsi_cfg->lane_num,
        .lane_bit_rate_mbps = dsi_cfg->lane_bit_rate_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "new_dsi_bus");
    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi, &io), TAG, "new_io_dbi");

    // 3) Create the generic DPI panel (host) — no vendor driver
    esp_lcd_dpi_panel_config_t dpi = dsi_cfg->dpi_config;
    esp_lcd_panel_handle_t dpi_panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi, &dpi_panel), TAG, "new_panel_dpi");

    // 4) Wrap with our custom ops (init sequence)
    jc8012_panel_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_INTERNAL);
    p->io = io;
    p->reset_gpio_num = dev_config->reset_gpio_num;
    p->base.del = NULL;
    p->base.reset = jc8012_reset;
    p->base.init = jc8012_init;
    p->base.draw_bitmap = esp_lcd_panel_draw_bitmap; // inherited from dpi host
    p->base.disp_on_off = jc8012_disp_on_off;
    p->base.set_gap = jc8012_set_gap;
    p->base.set_gap = jc8012_set_gap;
    p->base.color_space = dev_config->rgb_ele_order;
    p->base.rgb_elem_order = dev_config->rgb_ele_order;
    p->base.max_color_depth = 16;
    p->base.max_bpp = 16;
    p->base.max_transfer_sz = 800 * 1280 * 2;

    *ret = &p->base;
    return ESP_OK;
}
