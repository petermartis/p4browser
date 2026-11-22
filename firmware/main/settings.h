/*
 * p4browser settings (v5) — shared config defaults + API between
 * main.c (backlight, /brightness), remote.c (renderer endpoint) and
 * settings.c (the LVGL config screen).
 *
 * NVS layout:
 *   namespace "wifi"   : ssid (str), pass (str)        — STA credentials
 *   namespace "remote" : host (str), port (u16), url (str) — renderer endpoint
 *   namespace "ui"     : bright (u8)                   — backlight level 0-100
 *
 * Renderer defaults: the NAS (192.168.1.100) running nas-renderer on :9100;
 * the Chrome page it loads is the NAS-local dashboard on :5051 (the renderer
 * runs on the same host as the dashboard server, so 127.0.0.1 from the
 * renderer's point of view — the URL is loaded BY the renderer, not the board).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_DEFAULT_HOST "192.168.1.100"
#define CFG_DEFAULT_PORT 9100
#define CFG_DEFAULT_URL  "http://127.0.0.1:5051/"

/* ---- settings.c (LVGL config screen) ---- */
void  settings_open(void);      /* show the config screen (safe from any task) */
void  settings_open_boot(void); /* config screen + "no video yet" instructions (any task) */
void  settings_close(void);     /* back to the previous screen (any task) */
bool  settings_active(void);    /* is the config screen up right now */
void  settings_diag(char *buf, size_t len, size_t *o);   /* /state line */

/* ---- main.c (backlight, LEDC PWM on GPIO23) ---- */
int   backlight_get(void);      /* current level 0-100 */
void  backlight_set(int pct);   /* apply immediately + persist to NVS */
int   backlight_cfg_err(void);  /* LEDC config result: 0 ok, <0 failed (GPIO fallback) */

/* ---- remote.c (renderer endpoint) ---- */
void  remote_cfg_get(char *host, int host_len, int *port, char *url, int url_len);
void  remote_stop(void);        /* stop remote + touch tasks (any task, waits) */
void  remote_start(void);       /* (re)start them; no-op if already running */
void  remote_touch_reset(void); /* swallow the next release edge (settings close) */

/* ---- main.c (dashboard reload via the renderer's control endpoint) ----
 * The NAS renderer (renderer.js) runs a small HTTP control server next to the
 * frame-stream WS server. GET /reload re-fetches the pinned dashboard in the
 * running Chrome, so the board shows the new design WITHOUT a reboot. */
#define CFG_CONTROL_PORT 5052
bool  renderer_reload_do(void); /* GET <host>:CFG_CONTROL_PORT/reload; true on HTTP 200.
                                    BLOCKS up to ~6 s — call from a worker task, never the
                                    LVGL task (it would freeze the UI). */
