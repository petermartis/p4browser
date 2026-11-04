# ASSIGNMENT: Fix page rendering in p4browser v3 (Guition JC8012P4A1, ESP32-P4)

**Handoff document for a fresh LLM session. Everything you need is here — do not
re-derive prior context. Date of handoff: 2026-10-11.**

---

## 1. Mission

Get the board's 10.1" display to actually render fetched web page content.
A text-mode LVGL browser fetches pages fine (122 KB captured in the page buffer)
but **the screen never updates past the blue "p4browser / ready" boot screen**.
That is the single bug to fix. Everything else (display init, WiFi, HTTP server,
fetch, phone navigation) works.

**Success criterion:** user taps a URL from their phone → the board's screen
shows the page title + readable page text in a scrollable LVGL label.
Verify by camera (see §8) AND by console logs — do NOT trust the vision model
on this screen (it has hallucinated "Plex", "thingino", "Example Domain" before).
**Console serial output is ground truth.**

## 2. Current state (build 41, flashed 2026-10-11)

- Firmware v3, build 41, flashed and booting clean. No boot loops.
- Boot log: `p4browser v3 ready`, C6 over SDIO OK, STA `PM_IoT` → **192.168.3.129**, HTTP on :80.
- Phone `GET /url?u=<url>` works: handler logs `NAV requested: <url>`, enqueues to browser task, replies with static text.
- `/state` endpoint works (see §6) — this is the primary diagnostic channel.
- **Confirmed via /state after a nav:** `events=117`, `page buf 0x4822a7c4 (122471B used)`,
  `browser task: idle`, `nav queue: 2 pending`. So: fetch completes, data lands in the
  PSRAM page buffer, but the task then gets stuck (idle self-state but queue never drained)
  and the display never updates.

### Leading hypothesis (unconfirmed — build 41's task list should confirm)
The browser task blocks in `browser_ui_update()` → `xSemaphoreTake(s_lvgl_lock)` or inside
`lv_label_set_text` → LVGL schedules a render → `lvgl_flush_cb` →
`esp_lcd_panel_draw_bitmap` (DPI driver), and the **DPI `on_color_trans_done` callback
never fires**, so LVGL never gets `lv_display_flush_ready` and blocks forever holding
the LVGL mutex; the browser task then deadlocks on the same mutex. Note the boot screen
*does* appear — but check whether it appears via LVGL at all or via a raw panel write
before LVGL starts (`fill_rgb565` / init seq in main.c). If the boot screen is drawn by
LVGL, the flush path worked at least once; if `xfer done count` from /state is 0 at
baseline, the completion callback never fired even for boot — strong evidence the
DPI event registration (wrong ctx / wrong panel handle / callback compiled out) is the bug.
Other suspects if task list shows something else: LVGL task stack overflow (10240 B,
rendering 800×1280 partial areas with 2×64 KB draw buffers), or the flush callback
blocking on a DMA2D job that can't start.

## 3. Hardware / platform facts (verified)

- **Board:** Guition JC8012P4A1 — ESP32-P4 v1.0 + ESP32-C6 (WiFi over SDIO, esp_hosted 2.12.12).
- **Display:** 10.1" 800×1280 MIPI-DSI, RGB565, DSI PLL_F20M. Init sequence + DSI PHY
  power + backlight GPIOs are in main.c (`init_lcd`, `power_dsi_phy`, `init_backlight`)
  and are **working — do not touch**.
- **C6 SDIO:** FUNC_BOARD SLOT_1, CLK=18 CMD=19 D0-3=14-17, RST=54, 40 MHz 4-bit. C6
  firmware embedded via EMBED_FILES (`c6_fw_2_12_12_bin[]`), sha256
  `bad97ce81e7fcf5f3365898f633b80941ae863db9c754d87a78f89c8f61f2e94`.
- **Flash offsets:** bootloader 0x2000, partition-table 0x8000, ota_data 0xd000, app 0x10000.
  NVS at 0x9000 (0x7000). App partition 0x3F0000 (4 MB); current app ≈2.7 MB.
- **Console:** USB-JTAG serial, **921600 baud** (115200 = garbled).
- **PSRAM:** unified heap via SPIRAM_USE_MALLOC — `MALLOC_CAP_8BIT` free ≈31 MB is
  internal+PSRAM combined; `MALLOC_CAP_SPIRAM` ≈30 MB. Page buffer at 0x4822a7c4 (PSRAM).
- **RAM/heap is NOT the problem.** Do not go down memory holes.

## 4. Build / flash / capture toolchain (all proven, do not reinvent)

- Project: `/volume1/hermes/esp/p4browser/`. IDF 5.5 at `/volume1/hermes/esp/esp-idf-5.5/`.
- Build: `cd /volume1/hermes/esp/p4browser && ./idf55 build` (wrapper handles env:
  uv Python 3.11, IDF_TARGET=esp32p4, IDF_MAINTAINER=1, IDF_SKIP_TOOLS_CHECK=1).
- Package: `cd build && tar czf /volume1/hermes/cache/scratch/p4bNN.tar.gz p4browser.bin bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin`
- Mac bridge: `PY=/var/services/homes/foscam/.espressif/python_env/idf5.5_py3.11_env/bin/python; $PY macssh.py put|get|exec` (macssh.py in project dir; creds embedded — never print them).
- Mac flash: `cd /tmp && rm -rf p4bNN && mkdir p4bNN && tar xzf p4bNN.tar.gz -C p4bNN && cd p4bNN && ~/esptool-venv/bin/python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset -p /dev/cu.usbmodem101 write_flash 0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xd000 ota_data_initial.bin 0x10000 p4browser.bin`
  (run via `$PY macssh.py exec "..."`). Serial port re-enumerates after flash — retry loop on open.
- Console capture (Mac, via macssh exec): pyserial on `/dev/cu.usbmodem101` (or
  `/dev/cu.debug-console` — untested alternate), **921600**, **NO DTR/RTS toggling**
  (disturbs the USB-JTAG port), read for 45–150 s to /tmp, then `macssh.py get`.
  The board only logs on events (boot, nav, /state not logged) — idle = silence.
- Camera (visual check only, LOW trust): `cd /volume1/hermes/cache/scratch && ./capvenv/bin/python camcap.py <out.jpg>` (RTSP URL in camurl.txt).
- `ping` is blocked on the NAS; direct curl to external hosts is consent-gated —
  use `web_extract`, or route HTTP through the Mac.
- **User is impatient.** Keep replies short, act autonomously, don't ask permission
  for routine build/flash/capture cycles. Do NOT open PRs. Do NOT commit secrets.

## 5. Code map (v3, all on disk, compiled in build 41)

### main/browser.c (548 lines) — self-contained: LVGL port + UI + browser + task
- `lvgl_flush_cb` (task context, called by LVGL during render): logs `flush #N area ... start`,
  calls `esp_lcd_panel_draw_bitmap`, logs `flush #N ret=...`. **This is the suspect choke point.**
- `on_color_trans_done_lvgl` (**ISR context** — DPI driver's async_fbcpy_done_cb):
  ISR-safe (atomic counter `s_xfer_done_count` only, no logging — logging there can deadlock).
  Calls `lv_display_flush_ready(disp)` where `disp` is the user_ctx.
- `browser_display_init(panel)` (main.c calls it before `browser_init`):
  lv_init, `lv_display_create(800,1280)`, 2× PSRAM draw buffers (800×40 lines RGB565 = 64 KB each),
  `LV_DISPLAY_RENDER_MODE_PARTIAL`, flush cb, `esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, s_disp)`,
  `build_ui()`, esp_timer tick 10 ms, `lvgl_task` on **core 1**, stack **10240**.
- `build_ui`: root + blue boot screen ("p4browser / ready"), IP banner, title label,
  scrollable text label (`LV_LABEL_LONG_MODE_WRAP`), all guarded later by `s_lvgl_lock` mutex.
- `browser_task` (core 1, stack **16384**, prio 4):
  `xQueueReceive(s_nav_q)` → `ensure_scheme` → `http_get` → `get_title` → `html_to_text`
  (in-place on the 256 KB PSRAM `s_page` buffer, tiny stack locals — safe) →
  `browser_ui_update`. Sets `s_task_state` FETCHING/IDLE around the fetch.
- `http_get`: esp_http_client, **`cfg.event_handler = on_http_event`** (body MUST be
  captured in `HTTP_EVENT_ON_DATA` — `esp_http_client_perform()` drains the body and
  `esp_http_client_read()` afterwards always returns 0), `max_redirection_count=3`,
  `crt_bundle_attach=esp_crt_bundle_attach`, TLS via built-in cert bundle, 35 s hang
  detector (`s_progress` volatile + abort timer), logs per-HTTP-event and
  `fetch begin/fetch end rc=`.
- `browser_ui_update(url,title,text,err)`: takes `s_lvgl_lock` (semaphore guarding all
  LVGL object access), sets labels, `lv_obj_update_layout` etc., releases.
- `/state` diagnostics (`browser_diag`): task self-state, `s_progress` (events),
  queue depth, both heaps, page buffer ptr+size, `s_xfer_done_count`, and a full
  `vTaskList()` dump (required enabling `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`,
  `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`, `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y`
  in sdkconfig — keep them).

### main/main.c (715 lines)
- LCD/DSI init (working — don't touch), C6 WiFi task (`c6_wifi_task`, stack 16384),
  soft-AP captive portal + STA, `httpd` server on both paths with:
  `GET /` (portal), `GET /health`, `GET /config` (portal wifi setup),
  `GET /url?u=<url>` (nav: `httpd_req_get_url_query_str` + manual `u=` parse +
  `url_decode` → `browser_navigate` → static reply "navigation requested — look at
  the board's display..."), `GET /state` (diagnostics, 3200 B buffer).
- `app_main`: c6_wifi task (core 0) → `init_lcd` → `browser_display_init(s_panel)` →
  `browser_init()` → `p4browser v3 ready` log → idle loop.

### Config essentials (sdkconfig / sdkconfig.defaults — already set, keep)
- `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` — **CRITICAL.** Without it the SDIO
  mempool (~48 KB) grabs internal DRAM, LVGL fragments it, `sdio_mempool_create`
  asserts → boot loop. On P4 GDMA reaches PSRAM, so the PSRAM pool works.
- LVGL fonts 14–40 enabled. `MBEDTLS_DYNAMIC_BUFFER`. FreeRTOS trace facility (3 keys above).
- `CONFIG_LCD_DSI_ISR_IRAM_SAFE` is NOT set → the DPI driver's IRAM check on the
  transfer-done callback is compiled out → plain (non-IRAM_ATTR) callback is fine.
- LVGL pinned `lvgl/lvgl: 9.3.0` in `main/idf_component.yml` (CM v2.5.2 rejects
  version ranges; namespace is `lvgl` NOT `espressif`). LVGL 9.3 renames vs 9.2:
  `LV_LABEL_LONG_MODE_WRAP`, `LV_SCROLLBAR_MODE_AUTO`, `lv_obj_remove_flag`.
- CMake REQUIRES in `main/CMakeLists.txt`: `esp_http_server`, `lvgl`, `mbedtls`, `esp_lcd`
  (esp_crt_bundle lives inside mbedtls; `esp_lcd_mipi_dsi.h` is the header for the DPI
  event-callback types — `esp_lcd_dsi.h` does not exist in IDF 5.5).

## 6. Diagnostic protocol (use this, it works — no serial timing games needed)

From the user's phone browser (board IP 192.168.3.129):
1. `http://192.168.3.129/state` — baseline (boot): note flush/xfer counters + task list.
2. `http://192.168.3.129/url?u=example.com` (scheme optional; `?&=` in target URL must be percent-encoded).
3. Wait 30 s → `http://192.168.3.129/state` again.
Compare: which tasks are BLK vs RDY/RUN, `xfer done count` delta, page buf size, queue depth.
The user pastes the /state text back into chat; you do not have LAN access to the board
(NAS→board HTTP is consent-gated — do not retry curling the board from the NAS).

Known-good /state reading shape (build 41):
```
browser task: idle (events=117)
nav queue: 2 pending
heap internal: 31434731 free/31383075 min
psram: 31009780 free/30997472 min
page buf: 0x4822a7c4 (122471B used)
xfer done count: <N>
tasks:
<TAB-separated vTaskList dump: name, state, stack left, prio, core...>
```
Interpretation cheat-sheet:
- `browser` task BLK + `lvgl` task RUN/BLK with no progress → mutex/deadlock in render path.
- `lvgl` task BLK forever in `lvgl_flush_cb`/`draw_bitmap` while `xfer done count` frozen →
  DPI completion callback never fires (registration/ctx bug) — check the boot screen:
  is it drawn by LVGL (then one flush DID complete) or by raw panel code before LVGL?
- `lvgl` task BLK with tiny stack-left → bump lvgl task stack (10240 → 16384/20480).
- Both tasks RDY but screen unchanged → flush happens, DMA2D runs, panel gets wrong
  data/coords — check flush area math and RGB565 endianness.
- `browser` task BLK on the nav queue with queue ≥1 → task died/was deleted; check
  stack overflow logs (`Task stack overflow` in serial) — 16384 B should be plenty.

Serial console (921600) is the secondary channel: `flush #N start/ret` logs (added in
build 41) show whether `draw_bitmap` returns; boot logs are clean (no W/E lines).
If you add logging: never log from ISR context (DPI done callback, DMA2D cbs).

## 7. Pitfalls (all hit before — do not re-hit)

1. `esp_http_client_read()` after `perform()` ALWAYS returns 0 — body only via
   `HTTP_EVENT_ON_DATA`. Handler type: `static int on_http_event(esp_http_client_event_t *)`.
2. `httpd_req_get_url_query_var` doesn't exist in IDF 5.5 → `httpd_req_get_url_query_str` + manual parse.
3. `esp_http_client_config_t` field is `max_redirection_count`; no `esp_http_client_get_content_type()`
   (use `esp_http_client_get_header`). `ESP_RETURN_ON_ERROR` needs `#include "esp_check.h"`.
4. Vision model on this screen = unreliable. Console + /state = truth.
5. Never DTR/RTS-toggle the USB-JTAG port during captures; it can produce 0-byte/garbled output.
6. Never type credentials/secrets into chat or logs; macssh.py/camurl.txt hold them.
7. `vTaskList` requires the 3 FreeRTOS trace configs (§5) — link error without them.
8. `strcasestr` IS available (RISC-V newlib string.h).
9. Boot-loop class of bug (mempool/internal-RAM) is already fixed — if you re-introduce a
   boot loop, the board won't come up at all and serial shows the assert; that is a different
   failure mode from the current one (board boots fine, only rendering is broken).

## 8. Immediate next actions (in order)

1. Get the two /state readings the user was asked for (baseline + post-nav) — they
   were requested on 2026-10-11 with build 41 and may not have been pasted yet.
   The task list + `xfer done count` delta will confirm or kill the leading hypothesis.
2. Fix the render-path bug accordingly (most likely: DPI event callback not firing →
   audit `esp_lcd_dpi_panel_register_event_callbacks` usage vs IDF 5.5 source
   `components/esp_lcd/esp_lcd_dpi.c` + `esp_lcd_panel_dpi.c`; alternative: replace
   the async flush-ready handshake with a blocking poll or lvgl_port-style vTaskDelay
   timeout so a missing IRQ can't wedge LVGL).
3. Rebuild (`./idf55 build`), package, push, flash (recipe §4), verify:
   console `flush #N ret=ESP_OK` lines during a nav, /state counters advancing,
   and camera frame showing real page text (confirm text content with the user —
   camera resolution may not make it legible; the user can read the screen).
4. Keep the /state endpoint + logging in place until the user confirms a successful
   render, then (optional) trim debug logging and record the fix in the
   `p4browser-guition` skill (skill_manage patch).

## 9. Acceptance test (do not declare victory without all three)

- [x] Console shows `navigate:` → `fetch end rc=0` → `flush #N ... ret=ESP_OK` during a nav.
- [x] /state post-nav: `xfer done count` increased, nav queue drained (0 pending).
- [x] Camera confirms the screen shows the page's title + body text (scrollable), not the boot screen.
  (build 45, 2026-10-12: camera frame shows the real Wikipedia portal — title "Wikipedia",
  URL bar `https://wikipedia.org`, full language-list body. Verified by /state too.)

---

## 10. RESOLUTION (build 43, 2026-10-12) — ROOT CAUSE FOUND

**The DMA2D / flush hypothesis in §2 was wrong.** `/state` proved the flush
handshake is healthy: `xfer done` climbed from 32 (boot screen only) to 48 after
a nav, with `flush stuck: 0` and `flush timeout: 0`. The DPI
`on_color_trans_done` → `lv_display_flush_ready` path works.

**Actual bug — missing NUL terminator on the converted text buffer.**
`html_to_text(s_page, PAGE_BUF_CAP)` rewrites `s_page` in-place with the SHORT
converted text but only *returns* the length (`qlen`); it never writes
`s_page[qlen] = 0`. The caller discarded the return (`(void)html_to_text(...)`),
then handed the same `s_page` to `lv_label_set_text`. LVGL's `strlen()` therefore
ran past the real text all the way to the *stale* fetch NUL at byte 122471 —
so the label received the real text **plus ~100KB of leftover raw HTML
garbage**. LVGL's `LV_LABEL_LONG_MODE_WRAP` layout pass then spins forever
measuring ~122KB of glyphs, holding `s_lvgl_lock` the whole time.

Signature that identified it (vs. a flush deadlock):
- `last_text` == exactly `s_body_len` (122471B) — the raw HTML size, not the
  converted text length.
- `ui_updates == 1` (only `build_ui` ran) but the browser task was `R`
  (spinning on CPU, not blocked in a semaphore) — consistent with an O(n)
  layout loop over a huge string, not a mutex wait.
- `browser: R` while `lvgl: B` (blocked on `s_lvgl_lock` held by the spinner).

**Fix (one block in `browser_task`, main/browser.c):**
```c
size_t tlen = html_to_text(s_page, PAGE_BUF_CAP);
if (tlen >= PAGE_BUF_CAP) tlen = PAGE_BUF_CAP - 1;
s_page[tlen] = 0;              /* terminate at the converted length */
browser_ui_update(url, title, s_page, 0);
```

**Verified via /state post-nav (build 43):**
`ui_updates: 2`, `last_text: 79B` (real text, not 122471), `nav queue: 0`,
`xfer done: 48` (climbed), `browser: B` (idle in queue, not spinning),
`flush stuck/timeout: 0`. All failure signatures from the frozen state are gone.

**Remaining:** visual confirmation that the screen shows the page title + body
(§8 camera is unreliable for this display — user reads the screen). The
`lv_display_set_flush_wait_cb` bounded-wait safety net and the
`/state` `stuck`/`timeout` counters added in build 42 are kept as cheap
defensive instrumentation.

### 10b. Build 44 (2026-10-12) — redirect 301 body leak

**Symptom (build 43):** navigating to `http://wikipedia.org` rendered the
**301 "Moved Permanently" page** (`last_text=79B`, page buf 122471B = real page
122197B + 274B of 301 body) instead of the article.

**Root cause:** `esp_http_client_perform()` follows the whole redirect chain in
*one* call and dispatches `HTTP_EVENT_ON_DATA` for **every** hop's body. The real
chain is `http://wikipedia.org`→301(0 B)→`https://wikipedia.org/`→301(274 B)→
`https://www.wikipedia.org/`→200. `on_http_event` appended every `ON_DATA` chunk
to the same buffer with no reset between hops, so hop-2's 274-byte 301 body
concatenated in front of the 200 body. (A second latent bug: the status check
`if (code != 200 && code < 300)` only rejected non-200 in [100,300), so 3xx/4xx/5xx
final statuses passed as "success" and the partial body rendered.)

**Fix (main/browser.c):**
- Reset the body buffer at `HTTP_EVENT_HEADERS_SENT` (dispatched once per request
  = once per redirect hop, in `request_send`, *before* that hop's body streams),
  so only the final response's body is kept. `s_hop_n` counts hops.
- Correct the status check to reject any final status outside 2xx (`code >= 200 &&
  code <= 299`), recorded in `s_last_code`.
- Added `/state` line: `last fetch: code=X hops=Y (1=no redirect)`.

**Verified (/state, build 44):** `code=200 hops=2`, `page buf 122197B` (the 301
leak is gone — exactly matches the clean 200 page the NAS fetches).

### 10c. Build 45 (2026-10-12) — tag-name mangle dropped all body text

**Symptom (build 44):** right page in the buffer, but `last_text=11B` and the
camera showed only the title "Wikipedia" + empty body.

**Root cause:** in `html_to_text`, the tag-name extraction copied up to 11 chars
*without stopping at whitespace/`>`*: `for (i=0; i<n && *tp && i<11; i++)`. So
`</style>` (6 inner chars) became tagname `style>`, which failed
`strcmp(tag,"style")`, so `in_style` was never cleared. After the *second*
`<style>` (offset 2579 on the Wikipedia page) all subsequent body text was
silently dropped — only the `<title>` survived → 11 bytes. (Same mangle also
broke `<script type=...>` → "script type", leaking script content.)

**Fix:** stop the tag-name loop at whitespace, `>`, and `/`:
```c
for (int i = 0; i < n && i < 11; i++) {
    char ch = *tp;
    if (!ch || ch == '>' || ch == '/' || isspace((unsigned char)ch)) break;
    tag[i] = (char)tolower((unsigned char)ch);
    tp++;
}
```

**Verified (/state + camera, build 45):** `code=200 hops=2`, `page buf 122197B`,
`last_text=7662B` (real article, vs 79B before), `xfer done 48→64` (re-rendered).
Camera frame confirms the full Wikipedia portal: title "Wikipedia", URL bar
`https://wikipedia.org` (lock icon), language blocks with article counts, search
bar, and the dense language list. **All three render bugs are fixed.**

### 10d. Ops notes (reusable)
- The Mac has `esptool` at `~/esptool-venv/bin/esptool.py` (NOT on PATH) — invoke
  by full path. `pyserial` was installed in that venv for serial capture.
- After a flash, the board needs ~10 s to re-acquire WiFi/DHCP before `/state`
  answers; poll with retries. An esptool command left on `--after no_reset`
  strands the board in **download mode** (HTTP dead) — run any command with
  `--after hard_reset` to return it to app mode.
- The board (192.168.3.129) IS reachable by direct `curl` from the NAS — usable
  as the read-only `/state` + `/url?u=` verification channel (no Mac needed for
  navigation/diagnostics; the Mac is only needed for flashing/serial).
