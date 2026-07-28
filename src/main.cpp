#include <Arduino.h>
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h" // esp_ota_mark_app_valid_cancel_rollback() -- see setup()'s comment
#include "pins_config.h"
#include "hal/jd9165_lcd.h"
typedef jd9165_lcd board_lcd_t;
typedef bsp_lcd_handles_t board_lcd_handles_t;
#include "hal/gt911_touch.h"
#include "data/aircraft.h"
#include "data/fetcher.h"
#include "ui/status_bar.h"
#include "ui/location_picker.h"
#include "ui/views.h"
#include "ui/detail_card.h"
#include "ui/alerts.h"
#include "ui/settings.h"
#include "ui/range.h"
#include "ui/tile_cache.h"
#include "ui/map_view.h"
#include "ui/radar_view.h"
#include "ui/arrivals_view.h"
#include "ui/filters.h"
#include "ui/screensaver.h"
#include "ui/stats.h"
#include "ui/geo.h"
#include "data/storage.h"
#include "data/error_log.h"
#include "data/enrichment.h"
#include "data/locations.h"
#include "data/serial_config.h"
#include "data/ota.h"

// Global touch state — read by view timers to defer heavy rendering during interaction
volatile bool touch_active = false;

// Hardware drivers
static board_lcd_t lcd(LCD_RST);
static gt911_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

// Aircraft data
AircraftList aircraft_list;

// LVGL display
static lv_display_t *disp;
static uint16_t *buf0;
static uint16_t *buf1;

// Watchdog for a dropped flush-complete event -- see flush_ready_cb's
// comment. Plain volatiles, not a mutex: disp_flush_cb runs on the main
// task, flush_ready_cb runs in the DPI panel's ISR context, and both only
// ever do a single aligned write/read of these -- same pattern as this
// file's existing `touch_active`.
static volatile bool _flush_pending = false;
static volatile uint32_t _flush_started_at_ms = 0;

// Full-screen message + progress bar shown while an OTA download is in
// progress -- see loop()'s ota_status handling below. Map/Radar/Stats etc.
// all keep redrawing via their own LVGL timers throughout a download
// unless something stops them, and that concurrent redraw/flush traffic
// racing the flash write is the leading suspect for the bad visual
// glitching seen during a real update (this board's known PSRAM/DMA
// cache-coherency erratum, see README's Known Issues -- same root cause as
// the no-photos limitation). Freezing every *other* timer still cuts that
// traffic down a lot, but the progress bar itself needs its own periodic
// flush to actually move -- see the _last_shown_progress handling in
// loop() -- so some flashing during the update is expected and called out
// in the message itself rather than chasing a fully flash-free update.
static lv_obj_t *_ota_overlay = nullptr;
static lv_obj_t *_ota_bar = nullptr;
static lv_obj_t *_ota_pct_lbl = nullptr;
static OtaStatus _last_ota_status = OTA_IDLE;
static int _last_shown_progress = -1;

// Display flush callback
static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *color_map) {
    _flush_pending = true;
    _flush_started_at_ms = millis();
    lcd.lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint16_t *)color_map);
}

// Vsync callback — signals LVGL that flush is complete. If this ISR is ever
// missed (a dropped hardware event -- this exact chip already has a known
// PSRAM cache-coherency erratum, see the "no aircraft photos" known issue --
// or any other transient DMA/panel hiccup), lv_display_flush_ready() never
// fires, LVGL's flush state machine waits forever, and no future frame ever
// renders again -- silently, with nothing else in the app affected (same
// architectural gap as the touch driver's I2C-failure case: a single missed
// hardware completion event with no timeout anywhere). Confirmed on real
// hardware: display goes fully blank/black after some period of otherwise-
// normal runtime (well under an hour), independent of the touch issue.
// loop()'s watchdog (see below) detects a flush that's been pending too
// long and force-reboots rather than leaving the screen dead indefinitely --
// full restart, not an attempted in-place re-init, since safely tearing
// down and recreating the DSI bus/panel/LVGL association mid-session is far
// riskier than the touch driver's comparatively simple I2C reinit, and this
// project already uses a full ESP.restart() as the recovery path for other
// confirmed-stuck subsystems (see fetcher.cpp's SDIO crash handling).
static bool flush_ready_cb(esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
    _flush_pending = false;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

// Backlight level setter -- kept as a plain function so screensaver.cpp can
// drive the PWM level without needing the file-scope `lcd` instance exposed
// outside this file.
static void set_backlight(int percent) {
    lcd.example_bsp_set_lcd_backlight((uint32_t)percent);
}

// Touch read callback
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        touch_active = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        touch_active = false;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("ADS-B Display starting...");

    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Init I2C bus 1 (MUST be before touch.begin())
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)TP_I2C_SDA,
        .scl_io_num = (gpio_num_t)TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
    Serial.println("I2C bus initialized");

    // Init display hardware
    lcd.begin();
    Serial.println("LCD initialized");

    // Init touch hardware
    touch.begin();
    Serial.println("Touch initialized");

    // Init LVGL
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Allocate render buffers in PSRAM — 1/2 screen for PARTIAL mode
    // Larger buffers = fewer render passes per frame = better touch response
    uint32_t buf_size = LCD_H_RES * LCD_V_RES / 2;
    buf0 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    buf1 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(buf0 && buf1);

    // Create LVGL display — PARTIAL mode only redraws dirty regions
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf0, buf1, buf_size * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Register vsync callback for proper flush synchronization
    board_lcd_handles_t lcd_handles;
    lcd.get_handle(&lcd_handles);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = flush_ready_cb;
    esp_lcd_dpi_panel_register_event_callbacks(lcd_handles.panel, &cbs, disp);

    // Create LVGL touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    lv_indev_set_scroll_limit(indev, 10);

    // Poll touch at 10ms (vs 30ms default) — catches fast taps between render frames
    lv_timer_set_period(lv_indev_get_read_timer(indev), 10);

    // Init aircraft data
    aircraft_list.init();

    // Load config before UI so views init with the correct range
    g_config = storage_load_config();
    locations_init();

    // lcd.begin() (above) turned the backlight on at a hardcoded 100% before
    // g_config existed -- apply the user's saved level now.
    set_backlight(g_config.display_brightness_pct);

    // Create UI — LVGL must be fully set up before background tasks
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Range must be set up before the status bar -- it now owns the shared
    // range chip and reads range_label() once at creation time. Resume the
    // exact radius index last used (not range_set_default's "nearest to
    // radius_nm" approximation -- radius_nm is just the widest preset, we
    // have the literal last-used index now).
    range_set_levels(g_config.radius_presets, 4);
    range_set_index(g_config.last_range_idx);

    Serial.println("Creating status bar...");
    status_bar_create(screen);
    Serial.println("Status bar OK");

    Serial.println("views_init...");
    views_init(screen, &aircraft_list);
    Serial.println("views OK");

    // Must come after views_init — the tileview is created there, and as a
    // later sibling on `screen` it would otherwise render on top of (and
    // hide) this button.
    location_picker_init(screen);

    Serial.println("detail_card_init...");
    detail_card_init(screen, &aircraft_list);
    Serial.println("detail_card OK");

    Serial.println("alerts_init...");
    alerts_init(screen);
    Serial.println("alerts OK");

    Serial.println("settings_init...");
    settings_init(screen);
    Serial.println("settings OK");

    // OTA-in-progress overlay -- see loop()'s freeze/unfreeze handling and
    // the comment by _ota_overlay's declaration above. Created hidden,
    // fully opaque (unlike Settings' own semi-transparent overlay), and
    // moved to the very front so it's guaranteed to cover whatever view or
    // panel happens to be open when a download starts.
    _ota_overlay = lv_obj_create(screen);
    lv_obj_set_size(_ota_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_ota_overlay, 0, 0);
    lv_obj_set_style_bg_color(_ota_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_ota_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_ota_overlay, 0, 0);
    lv_obj_set_style_radius(_ota_overlay, 0, 0);
    lv_obj_clear_flag(_ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_ota_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *lbl = lv_label_create(_ota_overlay);
        lv_label_set_text(lbl,
            "Updating Firmware\n\n"
            "The screen may flash rapidly and unpredictably --\n"
            "this is expected and harmless. Do not unplug.\n\n"
            "It will restart automatically when finished.");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -60);

        _ota_bar = lv_bar_create(_ota_overlay);
        lv_obj_set_size(_ota_bar, 400, 24);
        lv_obj_align(_ota_bar, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_style_bg_color(_ota_bar, lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_bg_opa(_ota_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(_ota_bar, lv_color_hex(0x00cc66), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(_ota_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_bar_set_range(_ota_bar, 0, 100);
        lv_bar_set_value(_ota_bar, 0, LV_ANIM_OFF);

        _ota_pct_lbl = lv_label_create(_ota_overlay);
        lv_label_set_text(_ota_pct_lbl, "0%");
        lv_obj_set_style_text_color(_ota_pct_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(_ota_pct_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(_ota_pct_lbl, LV_ALIGN_CENTER, 0, 64);
    }

    status_bar_set_gear_callback([](lv_event_t *e) {
        settings_show();
    });

    settings_set_change_callback([](const UserConfig *cfg) {
        bool presets_changed = (memcmp(cfg->radius_presets, g_config.radius_presets,
                                       sizeof(g_config.radius_presets)) != 0);
        g_config = *cfg;
        range_set_levels(cfg->radius_presets, 4);
        if (presets_changed) {
            range_set_default(cfg->radius_nm);
            // Keep the resume-on-boot index in sync with the reset this just
            // triggered -- a one-off extra write, not a concern here since
            // this only runs once per Settings save.
            g_config.last_range_idx = range_get_index();
            storage_save_config(g_config);
        }
    });

    // Settings used to auto-open on first boot (no WiFi credentials in NVS)
    // to prompt setup -- removed now that the intended setup path is the
    // USB-serial config scripts (tools/configure_device.{sh,ps1}), not the
    // on-screen keyboard. Landing on a real view (Map by default, or
    // whatever views_resume_last_view() below resumes to) instead of an
    // unprompted Settings popover matters more once the device is meant to
    // be configured before it is ever handed a touchscreen tap.

    // Periodic status bar update -- aircraft count computed directly here
    // (opacity/filter/hide_ground/radius, matching what a view would draw)
    // rather than sourced from any one view's own drawn-count cache.
    // map_view_drawn_count() used to be the source, but it only updated
    // when Map's own canvas actually rendered a frame -- if resume-on-boot
    // (see views_resume_last_view()) lands on Radar/Arrivals/Stats, Map
    // might never draw at all, leaving the count stuck at 0 until the user
    // visits Map once (reported on hardware).
    lv_timer_create([](lv_timer_t *timer) {
        int count = 0;
        AircraftList *list = &aircraft_list;
        float center_lat, center_lon;
        locations_get_active_coords(&center_lat, &center_lon, nullptr);
        float radius_nm = range_get_nm();
        // Map deliberately draws (and lets you tap) aircraft beyond the
        // bullseye ring, out to its rectangular canvas edges -- extra
        // screen space used on purpose, unlike Radar's circular clip, which
        // is meant to look like a radar (see map_view_aircraft_visible()'s
        // own comment). views_filterable_index() also falls back to
        // VIEW_MAP when Stats is active, so this applies there too. Using
        // the plain radius check here for Map used to undercount relative
        // to what it actually drew (reported).
        bool is_map = (views_filterable_index() == VIEW_MAP);
        if (list->lock(pdMS_TO_TICKS(5))) {
            uint32_t now = millis();
            for (int i = 0; i < list->count; i++) {
                Aircraft &ac = list->aircraft[i];
                if (compute_aircraft_opacity(ac.stale_since, now) == 0) continue;
                if (!aircraft_passes_filter(ac)) continue;
                if (g_config.view_hide_ground[views_filterable_index()] && ac.on_ground) continue;
                if (is_map) {
                    if (!map_view_aircraft_visible(ac.lat, ac.lon)) continue;
                } else {
                    if (MapProjection::distance_nm(center_lat, center_lon, ac.lat, ac.lon) > radius_nm) continue;
                }
                count++;
            }
            list->unlock();
        }
        // stats_get()->current_count is the same "everything currently
        // tracked" figure the Stats screen shows under CURRENT TRAFFIC's
        // "Total: N" -- reused here rather than a second definition, so the
        // status bar and Stats screen can never silently disagree about
        // what "total" means.
        status_bar_update(fetcher_wifi_connected(), count, stats_get()->current_count, fetcher_last_update());
    }, 1000, nullptr);

    Serial.println("LVGL initialized - UI ready");
    Serial.printf("Heap free: %lu  PSRAM free: %lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    error_log_init();
    enrichment_init();
    fetcher_init(&aircraft_list);
    // tile_cache_init(); // disabled: lv_draw_image broken on ESP32-P4 PPA

    // Built last so its overlay (lv_obj_move_foreground() inside) sits above
    // every other popover/panel created above, whichever happens to be open
    // when the idle timeout fires.
    screensaver_init(screen, set_backlight);

    // See views_resume_last_view()'s own comment for its real ordering
    // constraint (must follow detail_card_init()/alerts_init(), both already
    // satisfied above) -- placed here, after fetcher_init(), simply so
    // resuming into Arrivals has live data to draw on the very first frame.
    views_resume_last_view();

    // Confirms this boot as good, canceling ESP-IDF's automatic OTA
    // rollback-on-failure (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the
    // precompiled sdkconfig -- confirmed, not assumed). A freshly OTA'd
    // image (data/ota.cpp) starts in a "pending verify" state; without this
    // call, the *next* reset for any reason -- including this board's known
    // occasional SDIO crash-reboot, unrelated to whether the update itself
    // was good -- would make the bootloader silently revert to the
    // previous firmware. Reaching this line means setup() completed
    // end-to-end (display, touch, WiFi/Ethernet init, fetcher tasks all
    // started) without crashing, which is a reasonable bar for "this build
    // isn't fundamentally broken" -- a no-op on any boot that wasn't a
    // pending OTA image.
    esp_ota_mark_app_valid_cancel_rollback();
}

void loop() {
    // OTA freeze/unfreeze -- plain loop() code, not an LVGL timer, so this
    // keeps running (and can detect the download ending) even while
    // lv_timer_enable(false) below has paused everything else. OTA_DONE
    // isn't handled as an "unfreeze" case -- ota.cpp calls ESP.restart()
    // itself right after reaching it, so the device reboots into the new
    // image before this would ever need to recover from that state.
    OtaStatus cur_ota_status = ota_status;
    if (cur_ota_status != _last_ota_status) {
        if (cur_ota_status == OTA_DOWNLOADING) {
            Serial.println("[OTA] Freezing UI for firmware download");
            lv_bar_set_value(_ota_bar, 0, LV_ANIM_OFF);
            lv_label_set_text(_ota_pct_lbl, "0%");
            lv_obj_move_foreground(_ota_overlay);
            lv_obj_clear_flag(_ota_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_refr_now(NULL); // flush the message once before freezing
            lv_timer_enable(false);
            _last_shown_progress = 0;
        } else if (_last_ota_status == OTA_DOWNLOADING) {
            Serial.println("[OTA] Update failed -- unfreezing UI");
            lv_timer_enable(true);
            lv_obj_add_flag(_ota_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(lv_screen_active());
        }
        _last_ota_status = cur_ota_status;
    }

    // Progress bar -- lv_timer_enable(false) above stops LVGL's own
    // periodic refresh along with everything else, so a bar update needs
    // its own explicit lv_refr_now() to actually reach the panel. This is
    // the one deliberate exception to "freeze everything": it's real,
    // wanted redraw traffic (see the message text warning about flashing),
    // just throttled to once per percentage point instead of every frame.
    if (cur_ota_status == OTA_DOWNLOADING && ota_progress != _last_shown_progress) {
        _last_shown_progress = ota_progress;
        lv_bar_set_value(_ota_bar, ota_progress, LV_ANIM_OFF);
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", ota_progress);
        lv_label_set_text(_ota_pct_lbl, pct);
        lv_refr_now(NULL);
    }

    lv_timer_handler();
    serial_config_poll();

    // Flush watchdog -- see flush_ready_cb's comment. 3000ms is generous
    // (a real frame completes in low tens of ms even under load) but still
    // catches a genuinely dead flush well before a user would sit staring
    // at a blank screen wondering if it's ever coming back.
    if (_flush_pending && (millis() - _flush_started_at_ms > 3000)) {
        Serial.println("Display flush stuck >3s (missed panel event?) -- rebooting");
        Serial.flush();
        ESP.restart();
    }

    // Heap monitor — log every 10s for crash diagnosis
    static uint32_t last_heap_log = 0;
    uint32_t now = millis();
    if (now - last_heap_log >= 10000) {
        last_heap_log = now;
        Serial.printf("HEAP int=%lu min=%lu  PSRAM=%lu\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // Yield briefly to FreeRTOS — 1ms instead of 5ms for better touch responsiveness
    vTaskDelay(pdMS_TO_TICKS(1));
}
