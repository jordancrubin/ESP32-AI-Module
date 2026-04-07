/*
  DisplayManager.cpp - ESP32 AI Module Display Controller
  Designed for ESP32-S3 with ILI9341 TFT and LVGL.
  Manages UI rendering, touch input, and screen transitions.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "DisplayManager.h"
#include <Fonts/FreeSans12pt7b.h>
#include <TJpg_Decoder.h>
#include "BootLogo.h"
#include <WiFi.h>
#include <time.h>
#include "SettingsManager.h"

// GPIO 38 conflicts with the PSRAM bus on S3 modules, causing audio distortion.
// GPIO 4 is a safe pin for PWM backlight control.
#define TFT_BL 4

// Static reference for the callback
static Arduino_GFX *static_gfx = nullptr;
static DisplayManager *static_dm = nullptr;
static TouchCalibration _currentCal;
static bool is_touch_active = false;
static bool touch_disabled = false;
bool g_isSubMenuActive = false;

extern bool isProcessing;
extern bool isSpeaking;
extern bool timerActive;
extern bool timerRinging;
extern unsigned long timerStartTime;
extern uint32_t timerDurationMs;

// Global state for Clock/Idle handling
static String g_lastVoice = "alloy";
static int g_lastVolume = 21;
static String g_voiceOptions = "alloy";
static String g_lastStatus = "Ready";
static lv_timer_t * g_clockTimer = nullptr;
static lv_timer_t * g_idleTimer = nullptr;
static uint32_t g_idleTimeout = 10000; // Dynamic idle timeout variable
static void showVoiceModelConfig();
static void showMicAecConfig();
static void setupBootScreen();
static lv_obj_t * boot_cont = nullptr;
static VoiceCallback g_voiceCb = nullptr;
static uint16_t * s_boot_buffer = nullptr;
static uint16_t s_boot_w = 0;
static uint16_t s_boot_h = 0;

// LVGL Async Wrappers for safe screen transitions
static void async_show_main_ui(void * p) {
    if (static_dm) static_dm->showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
}
static void async_show_voice_model_config(void *p) { showVoiceModelConfig(); }
static void async_show_audio_config(void *p) { if(static_dm) static_dm->showAudioConfig(); }
static void async_show_mic_aec_config(void *p) { showMicAecConfig(); }
static void async_show_web_config(void *p) { if(static_dm) static_dm->showWebConfig(WiFi.localIP().toString(), "aiesp.local"); }
static void async_show_wifi_config(void *p) { if(static_dm) static_dm->showWiFiConfig(); }

static void localVoiceEventHandler(lv_event_t * e) {
    if (g_voiceCb) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        char buf[32];
        lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
        g_voiceCb(String(buf));
    }
}

static lv_color_t getClockColor() {
    if (strcmp(settings.clockColor, "green") == 0) return lv_color_make(0, 255, 0);
    if (strcmp(settings.clockColor, "white") == 0) return lv_color_make(255, 255, 255);
    return lv_color_make(255, 0, 0); // Default Red
}

static void segment_draw_event_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    lv_draw_ctx_t * draw_ctx = lv_event_get_draw_ctx(e);
    
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    
    lv_draw_rect_dsc_t draw_dsc;
    lv_draw_rect_dsc_init(&draw_dsc);
    draw_dsc.bg_color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    draw_dsc.bg_opa = LV_OPA_COVER;
    
    lv_point_t points[6];
    
    if (w > h) { // Horizontal
        int32_t half_h = h / 2;
        points[0].x = coords.x1;                  points[0].y = coords.y1 + half_h;
        points[1].x = coords.x1 + half_h;         points[1].y = coords.y1;
        points[2].x = coords.x2 - half_h;         points[2].y = coords.y1;
        points[3].x = coords.x2;                  points[3].y = coords.y1 + half_h;
        points[4].x = coords.x2 - half_h;         points[4].y = coords.y2;
        points[5].x = coords.x1 + half_h;         points[5].y = coords.y2;
    } else { // Vertical
        int32_t half_w = w / 2;
        points[0].x = coords.x1 + half_w;         points[0].y = coords.y1;
        points[1].x = coords.x2;                  points[1].y = coords.y1 + half_w;
        points[2].x = coords.x2;                  points[2].y = coords.y2 - half_w;
        points[3].x = coords.x1 + half_w;         points[3].y = coords.y2;
        points[4].x = coords.x1;                  points[4].y = coords.y2 - half_w;
        points[5].x = coords.x1;                  points[5].y = coords.y1 + half_w;
    }
    
    lv_draw_polygon(draw_ctx, &draw_dsc, points, 6);
}

struct SevenSegmentDigit {
    lv_obj_t* segments[7]; // A, B, C, D, E, F, G
    
    lv_obj_t* create_segment(lv_obj_t* parent, int x, int y, int w, int h) {
        lv_obj_t* obj = lv_obj_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        // Set initial color to inactive gray
        lv_obj_set_style_bg_color(obj, lv_color_make(40, 40, 40), 0);
        lv_obj_add_event_cb(obj, segment_draw_event_cb, LV_EVENT_DRAW_MAIN, NULL);
        return obj;
    }

    void create(lv_obj_t* parent, int x, int y, int w, int h) {
        int thickness = w / 7; 
        int segLenH = w - 2 * thickness; 
        int segLenV = (h - 3 * thickness) / 2;
        
        // A: Top Horizontal
        segments[0] = create_segment(parent, x + thickness, y, segLenH, thickness);
        // B: Top Right Vertical
        segments[1] = create_segment(parent, x + w - thickness, y + thickness, thickness, segLenV);
        // C: Bottom Right Vertical
        segments[2] = create_segment(parent, x + w - thickness, y + 2 * thickness + segLenV, thickness, segLenV);
        // D: Bottom Horizontal
        segments[3] = create_segment(parent, x + thickness, y + h - thickness, segLenH, thickness);
        // E: Bottom Left Vertical
        segments[4] = create_segment(parent, x, y + 2 * thickness + segLenV, thickness, segLenV);
        // F: Top Left Vertical
        segments[5] = create_segment(parent, x, y + thickness, thickness, segLenV);
        // G: Middle Horizontal
        segments[6] = create_segment(parent, x + thickness, y + thickness + segLenV, segLenH, thickness);
    }

    void setNumber(int num) {
        // A=0, B=1, C=2, D=3, E=4, F=5, G=6
        const uint8_t patterns[10] = {
            0b00111111, // 0
            0b00000110, // 1
            0b01011011, // 2
            0b01001111, // 3
            0b01100110, // 4
            0b01101101, // 5
            0b01111101, // 6
            0b00000111, // 7
            0b01111111, // 8
            0b01101111  // 9
        };
        
        if (num < 0 || num > 9) return;
        uint8_t mask = patterns[num];
        
        for(int i=0; i<7; i++) {
            if ((mask >> i) & 1) {
                lv_obj_set_style_bg_color(segments[i], getClockColor(), 0); // Active Color
            } else {
                lv_obj_set_style_bg_color(segments[i], lv_color_make(40, 40, 40), 0); // Inactive Gray
            }
        }
    }
};

struct ClockWidgets {
    SevenSegmentDigit h1, h2, m1, m2;
    lv_obj_t* colon[2];
    lv_obj_t* wifiBars[4];
    lv_obj_t* dayLabels[7];
    lv_obj_t* weatherLabel;
    
    lv_obj_t* timerCont;
    SevenSegmentDigit tm1, tm2, ts1, ts2;
    lv_obj_t* timerColon[2];
};

static ClockWidgets g_clockWidgets;
static void showClockScreen();

static void kb_enable_timer_cb(lv_timer_t * timer) {
    touch_disabled = false;
    lv_timer_del(timer);
}

// LVGL Touchpad Read Callback
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
    if (!static_dm) return;

    if (touch_disabled) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t touchX, touchY;
    if (static_dm->getRawTouch(&touchX, &touchY)) {
        data->state = LV_INDEV_STATE_PR;
        is_touch_active = true;

        // Use saved calibration if valid, otherwise fallback to defaults
        uint16_t xMin = _currentCal.isValid ? _currentCal.xMin : 200;
        uint16_t xMax = _currentCal.isValid ? _currentCal.xMax : 3800;
        uint16_t yMin = _currentCal.isValid ? _currentCal.yMin : 200;
        uint16_t yMax = _currentCal.isValid ? _currentCal.yMax : 3800;

        data->point.x = map(touchX, xMin, xMax, 0, SCREEN_WIDTH);
        data->point.y = map(touchY, yMin, yMax, 0, SCREEN_HEIGHT);
    } else {
        data->state = LV_INDEV_STATE_REL;
        is_touch_active = false;
    }
}

// Callback function for TJpg_Decoder
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (static_gfx) {
        static_gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
    }
    return true;
}

// Callback for decoding JPG to memory buffer
static bool memory_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (s_boot_buffer) {
        for (int j = 0; j < h; j++) {
            memcpy(&s_boot_buffer[(y + j) * s_boot_w + x], &bitmap[j * w], w * 2);
        }
    }
    return true;
}

VolumeCallback DisplayManager::volumeCb = nullptr;
WiFiConfigCallback DisplayManager::wifiCb = nullptr;
APIConfigCallback DisplayManager::apiCb = nullptr;
APIUrlConfigCallback DisplayManager::apiUrlCb = nullptr;
AdminConfigCallback DisplayManager::adminCb = nullptr;
VoiceCallback DisplayManager::voiceCb = nullptr;
SetupModeCallback DisplayManager::setupModeCb = nullptr;
BalanceCallback DisplayManager::balanceCb = nullptr;

// LVGL Flush Callback
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    if (static_gfx) {
        uint32_t w = (area->x2 - area->x1 + 1);
        uint32_t h = (area->y2 - area->y1 + 1);
        static_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    }
    lv_disp_flush_ready(disp);
}

DisplayManager::DisplayManager() {
    bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
    gfx = new Arduino_ILI9341(bus, TFT_RST, 0, false);
    static_gfx = gfx; // Assign static pointer for callback
    static_dm = this;
    statusLabel = nullptr;
    audio_vu_l = nullptr;
    audio_vu_r = nullptr;
    spinner = nullptr;
    ts = nullptr;
    _lastVoice = "alloy";
    _lastVolume = 21;
    _voiceOptions = "alloy";
    _weatherTemp[0] = '\0';
    _weatherDesc[0] = '\0';
    _currentBrightness = 255;
}

void DisplayManager::begin(TouchCalibration cal) {
    _currentCal = cal;
    gfx->begin(20000000);
    gfx->setRotation(3);
    delay(100);

    // Initialize Backlight (PWM Mode)
    ledcAttach(TFT_BL, 5000, 8); // 5kHz, 8-bit resolution
    ledcWrite(TFT_BL, 255);      // Start at full brightness
    _currentBrightness = 255;

    gfx->setFont(&FreeSans12pt7b);

    // Ensure Display CS is HIGH (inactive) before starting Touch SPI
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);

    // Initialize Touch Screen (Shares SPI with Display)
    SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
    ts = new XPT2046_Touchscreen(TOUCH_CS, TOUCH_IRQ);
    ts->begin();
    ts->setRotation(3); // Match display rotation (Landscape)
    
    // Initialize LVGL
    lv_init();

    // Allocate draw buffer in PSRAM (1/10th of screen size is usually sufficient for buffering)
    static lv_color_t *buf = (lv_color_t *)ps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT / 10 * sizeof(lv_color_t));
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * SCREEN_HEIGHT / 10);

    // Initialize Display Driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Initialize Input Device Driver (Touchscreen)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    setupBootScreen();
}

// Callback to update the clock label every second
static void clock_update_cb(lv_timer_t * t) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    g_clockWidgets.h1.setNumber(timeinfo.tm_hour / 10);
    g_clockWidgets.h2.setNumber(timeinfo.tm_hour % 10);
    g_clockWidgets.m1.setNumber(timeinfo.tm_min / 10);
    g_clockWidgets.m2.setNumber(timeinfo.tm_min % 10);
    
    // Blink colon
    bool blink = (timeinfo.tm_sec % 2) == 0;
    lv_color_t col = blink ? getClockColor() : lv_color_make(40, 40, 40);
    if(g_clockWidgets.colon[0]) lv_obj_set_style_bg_color(g_clockWidgets.colon[0], col, 0);
    if(g_clockWidgets.colon[1]) lv_obj_set_style_bg_color(g_clockWidgets.colon[1], col, 0);

    // Update WiFi Signal
    int rssi = WiFi.RSSI();
    int level = 0;
    if (WiFi.status() == WL_CONNECTED) {
        if (rssi > -55) level = 4;
        else if (rssi > -65) level = 3;
        else if (rssi > -75) level = 2;
        else if (rssi > -85) level = 1;
    }
    
    for (int i = 0; i < 4; i++) {
        if (g_clockWidgets.wifiBars[i]) {
            // Active bars are Green, inactive are Dark Gray
            lv_color_t barColor = (i < level) ? lv_color_make(0, 255, 0) : lv_color_make(40, 40, 40);
            lv_obj_set_style_bg_color(g_clockWidgets.wifiBars[i], barColor, 0);
        }
    }

    // Update Day of Week
    int currentDay = 6; // 6=Sat (Temporarily forced) // timeinfo.tm_wday;
    int labelIdx = (currentDay + 6) % 7; // Convert to 0=Mon, 6=Sun
    for(int i=0; i<7; i++) {
        if(g_clockWidgets.dayLabels[i]) {
             if(i == labelIdx) {
                 lv_obj_set_style_text_color(g_clockWidgets.dayLabels[i], getClockColor(), 0);
             } else {
                 lv_obj_set_style_text_color(g_clockWidgets.dayLabels[i], lv_color_make(40, 40, 40), 0);
             }
        }
    }

    // Update Timer if active
    if (g_clockWidgets.timerCont) {
        if (timerActive || timerRinging) {
            lv_obj_clear_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_HIDDEN);
            uint32_t rem = 0;
            
            static int last_clock_sec = -1;
            static uint32_t display_rem = 0;
            static unsigned long last_start_time = 0;

            if (timerActive) {
                unsigned long elapsed = millis() - timerStartTime;
                uint32_t actual_rem = (elapsed < timerDurationMs) ? (timerDurationMs - elapsed) / 1000 : 0;
                
                // If a new timer just started, force an immediate visual update
                if (timerStartTime != last_start_time) {
                    display_rem = actual_rem;
                    last_start_time = timerStartTime;
                    last_clock_sec = timeinfo.tm_sec;
                }
                // Otherwise, only decrement the timer exactly when the clock ticks
                else if (timeinfo.tm_sec != last_clock_sec) {
                    display_rem = actual_rem;
                    last_clock_sec = timeinfo.tm_sec;
                }
                rem = display_rem;
            } else {
                display_rem = 0;
                last_start_time = 0;
            }
            
            int m = rem / 60;
            int s = rem % 60;
            if (m > 99) m = 99; // Cap at 99 mins
            
            g_clockWidgets.tm1.setNumber(m / 10);
            g_clockWidgets.tm2.setNumber(m % 10);
            g_clockWidgets.ts1.setNumber(s / 10);
            g_clockWidgets.ts2.setNumber(s % 10);
            
            lv_color_t timerCol = blink ? getClockColor() : lv_color_make(40, 40, 40);
            if (g_clockWidgets.timerColon[0]) lv_obj_set_style_bg_color(g_clockWidgets.timerColon[0], timerCol, 0);
            if (g_clockWidgets.timerColon[1]) lv_obj_set_style_bg_color(g_clockWidgets.timerColon[1], timerCol, 0);
            
            // Blink entire timer when ringing
            if (timerRinging && !blink) lv_obj_add_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update Weather Label if it exists
    if (g_clockWidgets.weatherLabel && static_dm) {
        if (strlen(static_dm->_weatherTemp) > 0) {
            lv_label_set_text_fmt(g_clockWidgets.weatherLabel, "%s %s", static_dm->_weatherTemp, static_dm->_weatherDesc);
        }
    }
}

// Callback when Clock screen is touched
static void clock_click_cb(lv_event_t * e) {
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    // Restore Main UI asynchronously to prevent use-after-free
    lv_async_call(async_show_main_ui, NULL);
}

// Callback to check for inactivity
static void idle_timer_cb(lv_timer_t * t) {
    // Prevent clock screen during active interactions
    if (isProcessing || isSpeaking) {
        lv_disp_trig_activity(NULL);
        return;
    }

    // If inactive for current timeout, switch to clock
    if (lv_disp_get_inactive_time(NULL) > g_idleTimeout) {
        if (g_idleTimer) {
             lv_timer_del(g_idleTimer);
             g_idleTimer = nullptr;
        }
        showClockScreen();
    }
}

static void showClockScreen() {
    g_isSubMenuActive = false;
    if (static_dm) {
        static_dm->resetUIPointers(); // Safely reset private pointers via public method
    }
    lv_obj_clean(lv_scr_act());
    boot_cont = nullptr;

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    // Container to center the clock
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, 300, 110);
    // Adjusted: Moved back left by ~8px (from -5 to -13) based on user feedback
    lv_obj_align(cont, LV_ALIGN_CENTER, -13, -5);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_EVENT_BUBBLE); // Allow clicks to pass through

    int dW = 50;
    int dH = 88;
    int gap = 13;
    int startX = 16; // Mathematically centered in 300px container
    int y = 6;

    g_clockWidgets.h1.create(cont, startX, y, dW, dH);
    g_clockWidgets.h2.create(cont, startX + dW + gap, y, dW, dH);
    
    // Colon
    int colonX = startX + 2 * dW + gap + 15; // Centered in the 41px gap
    g_clockWidgets.colon[0] = lv_obj_create(cont);
    lv_obj_set_size(g_clockWidgets.colon[0], 11, 11);
    lv_obj_set_style_radius(g_clockWidgets.colon[0], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_clockWidgets.colon[0], 0, 0);
    lv_obj_set_pos(g_clockWidgets.colon[0], colonX, y + dH/3);
    
    g_clockWidgets.colon[1] = lv_obj_create(cont);
    lv_obj_set_size(g_clockWidgets.colon[1], 11, 11);
    lv_obj_set_style_radius(g_clockWidgets.colon[1], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_clockWidgets.colon[1], 0, 0);
    lv_obj_set_pos(g_clockWidgets.colon[1], colonX, y + 2*dH/3);

    g_clockWidgets.m1.create(cont, startX + 2 * dW + 2 * gap + 28, y, dW, dH);
    g_clockWidgets.m2.create(cont, startX + 3 * dW + 3 * gap + 28, y, dW, dH);

    // Days Container (Above Clock)
    lv_obj_t * daysCont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(daysCont, 300, 20);
    // Moved down by 5px to bring it closer to the clock
    lv_obj_align_to(daysCont, cont, LV_ALIGN_OUT_TOP_MID, 8, 5);
    lv_obj_set_style_bg_opa(daysCont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(daysCont, 0, 0);
    lv_obj_set_style_pad_all(daysCont, 0, 0);
    lv_obj_clear_flag(daysCont, LV_OBJ_FLAG_SCROLLABLE);
    // Use Flex layout to evenly space variable-width day labels
    lv_obj_set_flex_flow(daysCont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(daysCont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    const char* dayNames[] = {"MON", "TUE", "WED", "THRS", "FRI", "SAT", "SUN"};
    for(int i=0; i<7; i++) {
        g_clockWidgets.dayLabels[i] = lv_label_create(daysCont);
        lv_label_set_text(g_clockWidgets.dayLabels[i], dayNames[i]);
        lv_obj_set_style_text_font(g_clockWidgets.dayLabels[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(g_clockWidgets.dayLabels[i], lv_color_make(40, 40, 40), 0);
    }

    // Timer Container (Top Mid)
    g_clockWidgets.timerCont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_clockWidgets.timerCont, 100, 36);
    lv_obj_align(g_clockWidgets.timerCont, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_opa(g_clockWidgets.timerCont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_clockWidgets.timerCont, 0, 0);
    lv_obj_set_style_pad_all(g_clockWidgets.timerCont, 0, 0);
    lv_obj_clear_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_EVENT_BUBBLE); // Pass clicks
    lv_obj_add_flag(g_clockWidgets.timerCont, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    int t_dW = 14, t_dH = 26, t_gap = 4, t_startX = 12, t_y = 5;
    g_clockWidgets.tm1.create(g_clockWidgets.timerCont, t_startX, t_y, t_dW, t_dH);
    g_clockWidgets.tm2.create(g_clockWidgets.timerCont, t_startX + t_dW + t_gap, t_y, t_dW, t_dH);

    int t_colonX = t_startX + 2 * t_dW + t_gap + 4;
    for(int i=0; i<2; i++) {
        g_clockWidgets.timerColon[i] = lv_obj_create(g_clockWidgets.timerCont);
        lv_obj_set_size(g_clockWidgets.timerColon[i], 4, 4);
        lv_obj_set_style_radius(g_clockWidgets.timerColon[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(g_clockWidgets.timerColon[i], 0, 0);
        lv_obj_set_pos(g_clockWidgets.timerColon[i], t_colonX, t_y + (i+1)*t_dH/3);
    }

    int t_tsX = t_colonX + 4 + 4;
    g_clockWidgets.ts1.create(g_clockWidgets.timerCont, t_tsX, t_y, t_dW, t_dH);
    g_clockWidgets.ts2.create(g_clockWidgets.timerCont, t_tsX + t_dW + t_gap, t_y, t_dW, t_dH);

    // WiFi Signal Meter (Top Right)
    lv_obj_t * wifiCont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifiCont, 40, 25);
    lv_obj_align(wifiCont, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_opa(wifiCont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifiCont, 0, 0);
    lv_obj_set_style_pad_all(wifiCont, 0, 0);
    lv_obj_clear_flag(wifiCont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifiCont, LV_OBJ_FLAG_EVENT_BUBBLE); // Pass clicks

    for(int i=0; i<4; i++) {
        g_clockWidgets.wifiBars[i] = lv_obj_create(wifiCont);
        int h = 6 + (i * 4); // Heights: 6, 10, 14, 18
        lv_obj_set_size(g_clockWidgets.wifiBars[i], 6, h);
        lv_obj_align(g_clockWidgets.wifiBars[i], LV_ALIGN_BOTTOM_LEFT, i * 9, 0);
        lv_obj_set_style_radius(g_clockWidgets.wifiBars[i], 2, 0);
        lv_obj_set_style_border_width(g_clockWidgets.wifiBars[i], 0, 0);
    }

    // Weather Label (Bottom Left)
    if (static_dm && strlen(static_dm->_weatherTemp) > 0) {
        g_clockWidgets.weatherLabel = lv_label_create(lv_scr_act());
        lv_label_set_text_fmt(g_clockWidgets.weatherLabel, "%s %s", static_dm->_weatherTemp, static_dm->_weatherDesc);
        lv_obj_set_style_text_font(g_clockWidgets.weatherLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(g_clockWidgets.weatherLabel, lv_color_make(200, 200, 200), 0);
        lv_obj_align(g_clockWidgets.weatherLabel, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    } else {
        g_clockWidgets.weatherLabel = nullptr;
    }

    // RUBINTECH Logo (Bottom Right)
    lv_obj_t * logo = lv_label_create(lv_scr_act());
    lv_label_set_text(logo, "RUBINTECH");
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logo, lv_color_make(40, 40, 40), 0);
    lv_obj_align(logo, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_EVENT_BUBBLE); // Pass clicks

    clock_update_cb(NULL); // Initial draw
    g_clockTimer = lv_timer_create(clock_update_cb, 500, NULL);

    // Add click event to the screen object to capture touches anywhere
    lv_obj_add_event_cb(lv_scr_act(), clock_click_cb, LV_EVENT_CLICKED, NULL);
}

static void showVoiceModelConfig() {
    g_isSubMenuActive = true;
    // Use public method to reset private pointers (statusLabel)
    if (static_dm) static_dm->showWiFiError("");
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    boot_cont = nullptr;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Voice & Model Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Back Button
    lv_obj_t * btnBack = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnBack, 50, 40);
    lv_obj_align(btnBack, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(btnBack, lv_color_make(60, 60, 60), 0);
    lv_obj_t * lblBack = lv_label_create(btnBack);
    lv_label_set_text(lblBack, LV_SYMBOL_LEFT);
    lv_obj_center(lblBack);
    lv_obj_add_event_cb(btnBack, [](lv_event_t * e){
        lv_async_call(async_show_main_ui, NULL);
    }, LV_EVENT_CLICKED, NULL);

    // Container for controls
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, 280, 160);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    // Parse options (Voice || Models)
    String vOpts = g_voiceOptions;
    String mOpts = "";
    int sep = g_voiceOptions.indexOf("||");
    if (sep != -1) {
        vOpts = g_voiceOptions.substring(0, sep);
        mOpts = g_voiceOptions.substring(sep + 2);
    }

    // Voice Label & Dropdown
    lv_obj_t * label_voice = lv_label_create(cont);
    lv_label_set_text(label_voice, "Voice");
    lv_obj_set_style_text_color(label_voice, lv_color_make(200, 200, 200), 0);
    lv_obj_align(label_voice, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * dd_voice = lv_dropdown_create(cont);
    lv_dropdown_set_options(dd_voice, vOpts.c_str());
    lv_obj_set_width(dd_voice, 240);
    lv_obj_align(dd_voice, LV_ALIGN_TOP_LEFT, 0, 25);
    
    // Select current voice
    int index = 0;
    int start = 0;
    int end = vOpts.indexOf('\n');
    while (end != -1 || start < vOpts.length()) {
        String opt = (end == -1) ? vOpts.substring(start) : vOpts.substring(start, end);
        if (opt == g_lastVoice) {
            lv_dropdown_set_selected(dd_voice, index);
            break;
        }
        index++;
        if (end == -1) break;
        start = end + 1;
        end = vOpts.indexOf('\n', start);
    }
    lv_obj_add_event_cb(dd_voice, localVoiceEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    // Model Label & Dropdown
    lv_obj_t * label_model = lv_label_create(cont);
    lv_label_set_text(label_model, "Model");
    lv_obj_set_style_text_color(label_model, lv_color_make(200, 200, 200), 0);
    lv_obj_align(label_model, LV_ALIGN_TOP_LEFT, 0, 70);

    lv_obj_t * dd_model = lv_dropdown_create(cont);
    lv_dropdown_set_options(dd_model, mOpts.c_str());
    lv_obj_set_width(dd_model, 240);
    lv_obj_align(dd_model, LV_ALIGN_TOP_LEFT, 0, 95);

    // Select current model
    // (Simple selection logic, assumes model ID is in the list)
    // ... (omitted for brevity, user can select manually)
    
    lv_obj_add_event_cb(dd_model, [](lv_event_t * e){
        if (g_voiceCb) {
            lv_obj_t * dropdown = lv_event_get_target(e);
            char buf[128];
            lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
            g_voiceCb("MODEL:" + String(buf));
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);
}

void DisplayManager::showMainUI(String currentVoice, int currentVolume, String voiceOptions) {
    g_isSubMenuActive = false;
    lv_disp_trig_activity(NULL); // Reset idle timer to keep screen awake
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    g_idleTimeout = 10000; // 10 seconds on Ready screen

    _lastVoice = currentVoice;
    _lastVolume = currentVolume;
    if (voiceOptions.length() > 0) _voiceOptions = voiceOptions;
    
    // Update globals for restoration from clock
    g_lastVoice = _lastVoice;
    g_lastVolume = _lastVolume;
    g_voiceOptions = _voiceOptions;

    boot_cont = nullptr; // Clear boot container reference
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    // Dark Theme Background
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);

    // --- Header / Top Right ---

    // Hamburger Dropdown Menu
    lv_obj_t * dd_menu = lv_dropdown_create(lv_scr_act());
    lv_dropdown_set_options(dd_menu, "Voice & Model\nAudio & Interrupt\nMic Mode & AEC\nSystem Setup");
    lv_dropdown_set_text(dd_menu, LV_SYMBOL_LIST); // Static icon text
    lv_dropdown_set_symbol(dd_menu, NULL); // Hide standard down arrow
    lv_dropdown_set_dir(dd_menu, LV_DIR_LEFT); // Expand to the left so it stays on screen
    lv_obj_set_size(dd_menu, 40, 40);
    lv_obj_align(dd_menu, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(dd_menu, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_radius(dd_menu, 20, 0); // Circle
    lv_obj_set_style_text_align(dd_menu, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_right(dd_menu, 0, 0); // Remove arrow padding to perfectly center the icon
    lv_obj_add_event_cb(dd_menu, [](lv_event_t * e){
        uint16_t idx = lv_dropdown_get_selected(lv_event_get_target(e));
        if (idx == 0) lv_async_call(async_show_voice_model_config, NULL);
        else if (idx == 1) lv_async_call(async_show_audio_config, NULL);
        else if (idx == 2) lv_async_call(async_show_mic_aec_config, NULL);
        else if (idx == 3 && static_dm) {
            if (settings.debugMode) Serial.println("Setup menu pressed. Web Server active.");
            if (DisplayManager::setupModeCb) DisplayManager::setupModeCb(true);
            lv_async_call(async_show_web_config, NULL);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // --- Volume Area (Top Left) ---

    // Volume Label
    lv_obj_t * label_vol = lv_label_create(lv_scr_act());
    lv_label_set_text(label_vol, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(label_vol, lv_color_make(200, 200, 200), 0);
    lv_obj_align(label_vol, LV_ALIGN_TOP_LEFT, 10, 18);

    // Volume Slider (Horizontal)
    lv_obj_t * slider_vol = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider_vol, 200); // Expanded width
    lv_obj_set_height(slider_vol, 10);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_LEFT, 40, 21);
    lv_slider_set_range(slider_vol, 0, 21);
    lv_slider_set_value(slider_vol, currentVolume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_vol, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_vol, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider_vol, volumeEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    // --- Brightness Area (Top Left, Below Volume) ---
    lv_obj_t * label_bri = lv_label_create(lv_scr_act());
    lv_label_set_text(label_bri, LV_SYMBOL_EYE_OPEN); // Eye icon representing display/visuals
    lv_obj_set_style_text_color(label_bri, lv_color_make(200, 200, 200), 0);
    lv_obj_align(label_bri, LV_ALIGN_TOP_LEFT, 10, 48);

    lv_obj_t * slider_bri = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider_bri, 200);
    lv_obj_set_height(slider_bri, 10);
    lv_obj_align(slider_bri, LV_ALIGN_TOP_LEFT, 40, 51);
    lv_slider_set_range(slider_bri, 10, 255);
    lv_slider_set_value(slider_bri, settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_bri, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_bri, lv_color_make(255, 200, 0), LV_PART_INDICATOR); // Yellow
    lv_obj_add_event_cb(slider_bri, [](lv_event_t * e){
        if (static_dm) {
            int val = lv_slider_get_value(lv_event_get_target(e));
            settings.brightness = val;
            static_dm->setBacklight(val); // Adjust hardware dynamically
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider_bri, [](lv_event_t * e){
        settings.save(); // Save to NVRAM safely on finger release
    }, LV_EVENT_RELEASED, NULL);

    // --- Status Area (Center/Bottom) ---

    // Status Container (Visual background for text, no interaction)
    lv_obj_t * statusCont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(statusCont, 300, 140); // Shorter to make room for brightness slider
    lv_obj_align(statusCont, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(statusCont, lv_color_make(50, 50, 50), 0);
    lv_obj_set_style_radius(statusCont, 10, 0);
    lv_obj_set_style_border_width(statusCont, 0, 0);
    lv_obj_clear_flag(statusCont, LV_OBJ_FLAG_SCROLLABLE);

    // Status Label (On Container)
    statusLabel = lv_label_create(statusCont);
    lv_label_set_text(statusLabel, g_lastStatus.c_str());
    lv_obj_set_width(statusLabel, 280);
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(statusLabel, lv_color_white(), 0);
    if (g_lastStatus.length() < 13) {
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_48, 0);
    } else if (g_lastStatus.length() < 20) {
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_28, 0);
    } else {
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    }
    
    // Start/Restart Idle Timer
    if (!g_idleTimer) {
        g_idleTimer = lv_timer_create(idle_timer_cb, 1000, NULL);
    }
}

void DisplayManager::resetUIPointers() {
    statusLabel = nullptr;
    audio_vu_l = nullptr;
    audio_vu_r = nullptr;
}

void DisplayManager::clear() {
    g_lastStatus = "";
    // LVGL handles background clearing automatically
    if (statusLabel) lv_label_set_text(statusLabel, "");
}

void DisplayManager::setBacklight(uint8_t brightness) {
    ledcWrite(TFT_BL, brightness);
    _currentBrightness = brightness;
}

void DisplayManager::fadeBacklight(uint8_t target, int durationMs) {
    int start = _currentBrightness;
    int steps = 50; // Number of steps for the fade
    int delayTime = durationMs / steps;
    
    for (int i = 1; i <= steps; i++) {
        int val = start + ((target - start) * i / steps);
        ledcWrite(TFT_BL, val);
        delay(delayTime);
    }
    ledcWrite(TFT_BL, target);
    _currentBrightness = target;
}

static void setupBootScreen() {
    // Use public method to reset private pointers (statusLabel)
    if (static_dm) static_dm->showWiFiError("");
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    
    boot_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(boot_cont, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(boot_cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(boot_cont, lv_color_black(), 0);
    lv_obj_set_style_border_width(boot_cont, 0, 0);
    lv_obj_set_flex_flow(boot_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(boot_cont, 10, 0);
    lv_obj_set_style_pad_gap(boot_cont, 5, 0);
}

void DisplayManager::showStatus(const char* message) {
    lv_disp_trig_activity(NULL); // Reset idle timer
    g_lastStatus = message;
    
    // If no UI is active (e.g. transitioning from WiFi config), recreate boot screen
    if (!boot_cont && !statusLabel && !g_clockTimer) {
        setupBootScreen();
    }

    if (boot_cont) {
        String m = String(message);
        String status = "";
        if (m.endsWith("|OK")) { status = "OK"; m = m.substring(0, m.length()-3); }
        else if (m.endsWith("|FAIL")) { status = "FAIL"; m = m.substring(0, m.length()-5); }

        lv_obj_t * row = lv_obj_create(boot_cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t * lbl = lv_label_create(row);
        lv_label_set_text(lbl, m.c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        if (status != "") {
            lv_obj_t * st = lv_label_create(row);
            lv_label_set_text(st, status == "OK" ? "[OK]" : "[ERROR]");
            lv_obj_set_style_text_color(st, status == "OK" ? lv_color_make(0, 255, 0) : lv_color_make(255, 0, 0), 0);
            lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
            lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_obj_update_layout(boot_cont);
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        lv_timer_handler(); // Force update
        return;
    }

    if (g_clockTimer) {
        showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
    }
    if (statusLabel) {
        lv_label_set_text(statusLabel, message);
        if (String(message).length() < 13) {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_48, 0);
        } else if (String(message).length() < 20) {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_28, 0);
        } else {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
        }
    }
}

void DisplayManager::showResponse(const String& response) {
    lv_disp_trig_activity(NULL); // Reset idle timer
    g_lastStatus = response;
    if (g_clockTimer) {
        showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
    }
    if (statusLabel) {
        lv_label_set_text(statusLabel, response.c_str());
        if (response.length() < 13) {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_48, 0);
        } else if (response.length() < 20) {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_28, 0);
        } else {
            lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
        }
    }
}

void DisplayManager::showBootLogo() {
    gfx->fillScreen(WHITE);
    
    // Check for JPEG Signature (FF D8)
    if (sizeof(boot_logo) > 2 && boot_logo[0] == 0xFF && boot_logo[1] == 0xD8) {
        // It is a JPEG
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false); // Standard for ILI9341
        TJpgDec.setCallback(tft_output);
        uint16_t w = 0, h = 0;
        if (TJpgDec.getJpgSize(&w, &h, boot_logo, sizeof(boot_logo)) == 0) {
            if (settings.debugMode) Serial.printf("Boot Logo Size: %dx%d\n", w, h);
            int x = (gfx->width() - w) / 2;
            int y = (gfx->height() - h) / 2;
            TJpgDec.drawJpg(x, y, boot_logo, sizeof(boot_logo));
        } else {
            if (settings.debugMode) Serial.println("Boot Logo Error: Invalid JPG data.");
        }
    } else {
        if (settings.debugMode) Serial.println("Boot Logo Error: Unknown format.");
    }
}

void DisplayManager::showThinking(bool active) {
    lv_disp_trig_activity(NULL); // Reset idle timer
    if (g_clockTimer) {
        showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
    }
    if (spinner) {
        if (active) {
            lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DisplayManager::setWiFiConfigCallback(WiFiConfigCallback cb) {
    wifiCb = cb;
}

void DisplayManager::showWiFiConfig() {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    boot_cont = nullptr;
    statusLabel = nullptr;
    
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "WiFi Configuration");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

    wifi_dd = lv_dropdown_create(lv_scr_act());
    lv_dropdown_set_options(wifi_dd, "Scanning...");
    lv_obj_set_width(wifi_dd, 280);
    lv_obj_align(wifi_dd, LV_ALIGN_TOP_MID, 0, 50);

    wifi_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, false);
    lv_textarea_set_placeholder_text(wifi_ta, "Password");
    lv_obj_set_width(wifi_ta, 280);
    lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, wifi_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(wifi_ta, [](lv_event_t* e){
        lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, kb);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 85, 35);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Connect");
    lv_obj_add_event_cb(btn, wifiConfigEventHandler, LV_EVENT_CLICKED, this);

    // Force a UI draw so the user sees the screen before the blocking scan starts
    lv_timer_handler();

    int n = WiFi.scanNetworks();
    String opts = "";
    for (int i = 0; i < n; i++) {
        opts += WiFi.SSID(i);
        if (i < n - 1) opts += "\n";
    }
    lv_dropdown_set_options(wifi_dd, opts.c_str());
}

void DisplayManager::setAPIConfigCallback(APIConfigCallback cb) {
    apiCb = cb;
}

void DisplayManager::showAPIConfig(String currentKey) {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;
    
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "API Key Configuration");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

    lv_obj_t * prefix = lv_label_create(lv_scr_act());
    lv_label_set_text(prefix, "sk-");
    lv_obj_align(prefix, LV_ALIGN_TOP_LEFT, 10, 60);

    lv_obj_t * api_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(api_ta, true);
    lv_textarea_set_password_mode(api_ta, false);
    lv_textarea_set_placeholder_text(api_ta, "key...");
    lv_textarea_set_accepted_chars(api_ta, "abcdefghijklmnopqrstuvwxyz0123456789");
    lv_obj_set_width(api_ta, 260);
    lv_obj_align(api_ta, LV_ALIGN_TOP_LEFT, 45, 50);

    if (currentKey.length() > 0) {
        if (currentKey.startsWith("sk-")) currentKey = currentKey.substring(3);
        lv_textarea_set_text(api_ta, currentKey.c_str());
    }

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, api_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // Letter Layout (Lower Case)
    static const char * kb_map_lc[] = {
        " ", "123", "\n",
        "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
        "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
        "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, ""
    };
    static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
        LV_BTNMATRIX_CTRL_HIDDEN | 8, 2,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 2
    };

    // Number Layout (Large Keys)
    static const char * kb_map_num[] = {
        "abc", " ", "\n",
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "0", LV_SYMBOL_BACKSPACE, ""
    };
    static const lv_btnmatrix_ctrl_t kb_ctrl_num[] = {
        1, LV_BTNMATRIX_CTRL_HIDDEN | 2,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1
    };

    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_map_num, kb_ctrl_num);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);

    // Handle mode switching (123 <-> abc)
    lv_obj_add_event_cb(kb, [](lv_event_t* e){
        lv_obj_t* kb = lv_event_get_target(e);
        const char* txt = lv_btnmatrix_get_btn_text(kb, lv_btnmatrix_get_selected_btn(kb));
        if (!txt) return;

        if (strcmp(txt, "123") == 0) {
            lv_obj_t* ta = lv_keyboard_get_textarea(kb);
            if (ta) {
                const char* curr = lv_textarea_get_text(ta);
                size_t len = strlen(curr);
                if (len >= 3 && strcmp(curr + len - 3, "123") == 0) {
                    for(int i=0; i<3; i++) lv_textarea_del_char(ta);
                }
            }
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
            touch_disabled = true;
            lv_timer_create(kb_enable_timer_cb, 1000, NULL);
        } else if (strcmp(txt, "abc") == 0) {
            lv_obj_t* ta = lv_keyboard_get_textarea(kb);
            if (ta) {
                const char* curr = lv_textarea_get_text(ta);
                size_t len = strlen(curr);
                if (len >= 3 && strcmp(curr + len - 3, "abc") == 0) {
                    for(int i=0; i<3; i++) lv_textarea_del_char(ta);
                }
            }
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
            touch_disabled = true;
            lv_timer_create(kb_enable_timer_cb, 1000, NULL);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(api_ta, [](lv_event_t* e){
        lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, kb);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 85, 35);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_add_event_cb(btn, apiConfigEventHandler, LV_EVENT_CLICKED, api_ta);
}

void DisplayManager::setAPIUrlConfigCallback(APIUrlConfigCallback cb) {
    apiUrlCb = cb;
}

void DisplayManager::showAPIUrlConfig(String currentUrl) {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;
    
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "API URL Configuration");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 5);

    lv_obj_t * prefix = lv_label_create(lv_scr_act());
    lv_label_set_text(prefix, "http://");
    lv_obj_align(prefix, LV_ALIGN_TOP_LEFT, 10, 45);

    lv_obj_t * url_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(url_ta, true);
    lv_textarea_set_password_mode(url_ta, false);
    lv_textarea_set_placeholder_text(url_ta, "host:port");
    lv_obj_set_width(url_ta, 230);
    lv_obj_align(url_ta, LV_ALIGN_TOP_LEFT, 70, 35);

    if (currentUrl.length() > 0) {
        if (currentUrl.startsWith("http://")) currentUrl = currentUrl.substring(7);
        int suffixIndex = currentUrl.indexOf("/api/chat/completions");
        if (suffixIndex != -1) currentUrl = currentUrl.substring(0, suffixIndex);
        lv_textarea_set_text(url_ta, currentUrl.c_str());
    }

    lv_obj_t * suffix = lv_label_create(lv_scr_act());
    lv_label_set_text(suffix, "/api/chat/completions");
    lv_obj_align(suffix, LV_ALIGN_TOP_LEFT, 70, 75);

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, url_ta);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 85, 35);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_add_event_cb(btn, apiUrlConfigEventHandler, LV_EVENT_CLICKED, url_ta);
}

void DisplayManager::setAdminConfigCallback(AdminConfigCallback cb) {
    adminCb = cb;
}

void DisplayManager::showAdminConfig() {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;
    
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Set Admin Password");
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

    lv_obj_t * pass_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(pass_ta, true);
    lv_textarea_set_password_mode(pass_ta, false); // Visible for setting
    lv_textarea_set_placeholder_text(pass_ta, "Password");
    lv_obj_set_width(pass_ta, 280);
    lv_obj_align(pass_ta, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, pass_ta);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 85, 35);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Save");
    lv_obj_add_event_cb(btn, adminConfigEventHandler, LV_EVENT_CLICKED, pass_ta);
}

void DisplayManager::showWebConfig(String ip, String hostname) {
    g_isSubMenuActive = true;
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }
    g_idleTimeout = 60000; // 60 seconds on sub-screens

    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);
    
    // Check if configuration is incomplete (default values or empty)
    bool incomplete = (strlen(settings.apiKey) == 0 || strcmp(settings.apiKey, "your_api_key_here") == 0 ||
                       strlen(settings.apiUrl) == 0 || strcmp(settings.apiUrl, "http://your-api-endpoint/api/chat/completions") == 0 ||
                       strlen(settings.wifiSSID) == 0 || strcmp(settings.wifiSSID, "YOUR_WIFI_SSID") == 0);

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, incomplete ? "Configuration Required" : "Configuration Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_make(180, 180, 180), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * connect_label = lv_label_create(lv_scr_act());
    String connectStr = "Connect to: http://" + hostname + " (" + ip + ")";
    lv_label_set_text(connect_label, connectStr.c_str());
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(connect_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(connect_label, LV_ALIGN_TOP_LEFT, 10, 50);

    lv_obj_t * note = lv_label_create(lv_scr_act());
    lv_label_set_text(note, "Use Admin Password to login");
    lv_obj_set_style_text_color(note, lv_color_make(180, 180, 180), 0);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 10, 75);

    lv_obj_t * update_label = lv_label_create(lv_scr_act());
    String updateStr = "OTA Update: http://" + hostname + "/update";
    lv_label_set_text(update_label, updateStr.c_str());
    lv_obj_set_style_text_font(update_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(update_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(update_label, LV_ALIGN_TOP_LEFT, 10, 100);

    // Debug Toggle Row (Left Aligned)
    lv_obj_t * sw_lbl = lv_label_create(lv_scr_act());
    lv_label_set_text(sw_lbl, "Debug");
    lv_obj_set_style_text_color(sw_lbl, lv_color_make(180, 180, 180), 0);
    lv_obj_align(sw_lbl, LV_ALIGN_BOTTOM_LEFT, 20, -25);

    lv_obj_t * sw = lv_switch_create(lv_scr_act());
    lv_obj_set_size(sw, 40, 20);
    lv_obj_align_to(sw, sw_lbl, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    if (settings.debugMode) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, [](lv_event_t * e){
        lv_obj_t * obj = lv_event_get_target(e);
        settings.debugMode = lv_obj_has_state(obj, LV_STATE_CHECKED);
        settings.save();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * baud_lbl = lv_label_create(lv_scr_act());
    lv_label_set_text(baud_lbl, "115200");
    lv_obj_set_style_text_font(baud_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(baud_lbl, lv_color_make(150, 150, 150), 0);
    lv_obj_align_to(baud_lbl, sw, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // Close Button
    if (!incomplete) {
        lv_obj_t *btnClose = lv_btn_create(lv_scr_act());
        lv_obj_set_size(btnClose, 80, 40);
        lv_obj_align(btnClose, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
        lv_obj_t *lblClose = lv_label_create(btnClose);
        lv_label_set_text(lblClose, "Return");
        lv_obj_center(lblClose);
        lv_obj_add_event_cb(btnClose, closeSetupEventHandler, LV_EVENT_CLICKED, this);
    }
}

void DisplayManager::showWiFiError(const char* message) {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    audio_vu_l = nullptr; audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, message);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Configure WiFi");
    lv_obj_add_event_cb(btn, wifiRetryHandler, LV_EVENT_CLICKED, NULL);
}

void DisplayManager::wifiConfigEventHandler(lv_event_t * e) {
    DisplayManager* dm = (DisplayManager*)lv_event_get_user_data(e);
    if (dm && wifiCb) {
        char ssid[64];
        lv_dropdown_get_selected_str(dm->wifi_dd, ssid, sizeof(ssid));
        wifiCb(String(ssid), String(lv_textarea_get_text(dm->wifi_ta)));
    }
}

void DisplayManager::apiConfigEventHandler(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (apiCb) {
        apiCb("sk-" + String(lv_textarea_get_text(ta)));
    }
}

void DisplayManager::apiUrlConfigEventHandler(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (apiUrlCb) {
        apiUrlCb(String(lv_textarea_get_text(ta)));
    }
}

void DisplayManager::adminConfigEventHandler(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (adminCb) {
        adminCb(String(lv_textarea_get_text(ta)));
    }
}

void DisplayManager::setupEventHandler(lv_event_t * e) {
    DisplayManager* dm = (DisplayManager*)lv_event_get_user_data(e);
    if (dm) {
        if (settings.debugMode) Serial.println("Setup button pressed. Web Server is active.");
        if (setupModeCb) setupModeCb(true);
        lv_async_call(async_show_web_config, NULL);
    }
}

void DisplayManager::closeSetupEventHandler(lv_event_t * e) {
    DisplayManager* dm = (DisplayManager*)lv_event_get_user_data(e);
    if (dm) {
        if (setupModeCb) setupModeCb(false);
        lv_async_call(async_show_main_ui, NULL);
    }
}

void DisplayManager::setVoiceCallback(VoiceCallback cb) {
    voiceCb = cb;
    g_voiceCb = cb;
}

void DisplayManager::setSetupModeCallback(SetupModeCallback cb) {
    setupModeCb = cb;
}

void DisplayManager::voiceEventHandler(lv_event_t * e) {
    if (voiceCb) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        char buf[32];
        lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
        voiceCb(String(buf));
    }
}

void DisplayManager::wifiRetryHandler(lv_event_t * e) {
    lv_async_call(async_show_wifi_config, NULL);
}

void DisplayManager::setVolumeCallback(VolumeCallback cb) {
    volumeCb = cb;
}

void DisplayManager::volumeEventHandler(lv_event_t * e) {
    if (volumeCb) {
        lv_obj_t * slider = lv_event_get_target(e);
        volumeCb(lv_slider_get_value(slider));
    }
}

bool DisplayManager::getRawTouch(uint16_t *x, uint16_t *y) {
    if (ts && (ts->touched() || ts->getPoint().z > 500)) {
        TS_Point p = ts->getPoint();
        *x = p.x;
        *y = p.y;
        return true;
    }
    return false; 
}

void DisplayManager::calibrateTouch(TouchCalibration& cal) {
    gfx->fillScreen(BLACK);
    gfx->setFont(NULL); // Use system font for smaller text
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    
    auto waitForTouch = [&](uint16_t x, uint16_t y, uint16_t &rawX, uint16_t &rawY) {
        gfx->fillCircle(x, y, 5, RED);
        gfx->setCursor(x > SCREEN_WIDTH/2 ? x - 70 : x + 10, y > SCREEN_HEIGHT/2 ? y - 15 : y + 10);
        gfx->print("Touch Dot");
        
        // Wait for touch indefinitely
        unsigned long lastBeat = 0;
        while(true) {
            if (Serial.available()) {
                if (settings.debugMode) Serial.println("\nCalibration aborted via Serial.");
                return false;
            }
            
            if (getRawTouch(&rawX, &rawY)) {
                gfx->fillCircle(x, y, 5, GREEN);
                delay(500); // Debounce
                return true;
            }
            if (millis() - lastBeat > 1000) { Serial.print("."); lastBeat = millis(); }
            delay(10);
        }
    };

    uint16_t rx[4], ry[4];
    
    // Point 1: Top Left
    if (!waitForTouch(20, 20, rx[0], ry[0])) {
        gfx->setFont(&FreeSans12pt7b);
        return;
    }
    
    gfx->fillScreen(BLACK);
    gfx->setFont(NULL);
    delay(500);
    
    // Point 2: Top Right
    if (!waitForTouch(SCREEN_WIDTH - 20, 20, rx[1], ry[1])) {
        gfx->setFont(&FreeSans12pt7b);
        return;
    }

    gfx->fillScreen(BLACK);
    gfx->setFont(NULL);
    delay(500);

    // Point 3: Bottom Right
    if (!waitForTouch(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, rx[2], ry[2])) {
        gfx->setFont(&FreeSans12pt7b);
        return;
    }

    gfx->fillScreen(BLACK);
    gfx->setFont(NULL);
    delay(500);

    // Point 4: Bottom Left
    if (!waitForTouch(20, SCREEN_HEIGHT - 20, rx[3], ry[3])) {
        gfx->setFont(&FreeSans12pt7b);
        return;
    }

    // Calculate averages for sides
    int avg_left_x = (rx[0] + rx[3]) / 2;
    int avg_right_x = (rx[1] + rx[2]) / 2;
    int avg_top_y = (ry[0] + ry[1]) / 2;
    int avg_bottom_y = (ry[2] + ry[3]) / 2;

    // Extrapolate to edges (0 and Width/Height) based on the 20px margin
    // Formula: Edge = Measured - (Slope * Margin)
    cal.xMin = constrain(avg_left_x - (avg_right_x - avg_left_x) * 20 / (SCREEN_WIDTH - 40), 0, 4095);
    cal.xMax = constrain(avg_right_x + (avg_right_x - avg_left_x) * 20 / (SCREEN_WIDTH - 40), 0, 4095);
    cal.yMin = constrain(avg_top_y - (avg_bottom_y - avg_top_y) * 20 / (SCREEN_HEIGHT - 40), 0, 4095);
    cal.yMax = constrain(avg_bottom_y + (avg_bottom_y - avg_top_y) * 20 / (SCREEN_HEIGHT - 40), 0, 4095);
    cal.isValid = true;
    
    // Update the internal static copy so the LVGL callback uses new values immediately
    _currentCal = cal;

    gfx->setFont(&FreeSans12pt7b);
    gfx->fillScreen(BLACK);
    gfx->setCursor(20, SCREEN_HEIGHT/2);
    gfx->print("Calibration Saved!");
    delay(1000);
}

void DisplayManager::updateWeather(const char* temp, const char* desc) {
    strlcpy(_weatherTemp, temp, sizeof(_weatherTemp));
    strlcpy(_weatherDesc, desc, sizeof(_weatherDesc));
}

void DisplayManager::showAudioConfig() {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);
    
    // Reset pointers
    audio_vu_l = nullptr;
    audio_vu_r = nullptr;
    statusLabel = nullptr;
    boot_cont = nullptr;

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Audio & Interrupt Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // VU Meters
    // If Stereo (0) or Left (1), show Left
    if (settings.micMode == 0 || settings.micMode == 1) {
        audio_vu_l = lv_bar_create(lv_scr_act());
        lv_obj_set_size(audio_vu_l, 200, 15);
        lv_obj_align(audio_vu_l, LV_ALIGN_TOP_MID, 0, 40);
        lv_bar_set_range(audio_vu_l, 0, 10000); // Scaled for normal speech instead of absolute max
        lv_obj_set_style_bg_color(audio_vu_l, lv_color_make(40, 40, 40), LV_PART_MAIN);
        lv_obj_set_style_bg_color(audio_vu_l, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
        
        lv_obj_t * l_lbl = lv_label_create(lv_scr_act());
        lv_label_set_text(l_lbl, "L");
        lv_obj_align_to(l_lbl, audio_vu_l, LV_ALIGN_OUT_LEFT_MID, -5, 0);
        lv_obj_set_style_text_color(l_lbl, lv_color_white(), 0);
    }

    // If Stereo (0) or Right (2), show Right
    if (settings.micMode == 0 || settings.micMode == 2) {
        audio_vu_r = lv_bar_create(lv_scr_act());
        lv_obj_set_size(audio_vu_r, 200, 15);
        int y_offset = (settings.micMode == 0) ? 65 : 40;
        lv_obj_align(audio_vu_r, LV_ALIGN_TOP_MID, 0, y_offset);
        lv_bar_set_range(audio_vu_r, 0, 10000); // Scaled for normal speech instead of absolute max
        lv_obj_set_style_bg_color(audio_vu_r, lv_color_make(40, 40, 40), LV_PART_MAIN);
        lv_obj_set_style_bg_color(audio_vu_r, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);

        lv_obj_t * r_lbl = lv_label_create(lv_scr_act());
        lv_label_set_text(r_lbl, "R");
        lv_obj_align_to(r_lbl, audio_vu_r, LV_ALIGN_OUT_LEFT_MID, -5, 0);
        lv_obj_set_style_text_color(r_lbl, lv_color_white(), 0);
    }

    // Balance Slider
    lv_obj_t * label_bal = lv_label_create(lv_scr_act());
    lv_label_set_text(label_bal, "Input Balance");
    lv_obj_set_style_text_color(label_bal, lv_color_white(), 0);
    lv_obj_align(label_bal, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t * slider_bal = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider_bal, 200);
    lv_obj_align(slider_bal, LV_ALIGN_CENTER, 0, 15);
    lv_slider_set_range(slider_bal, -100, 100);
    lv_slider_set_value(slider_bal, settings.inputBalance, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_bal, balanceEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * lbl_l = lv_label_create(lv_scr_act());
    lv_label_set_text(lbl_l, "L");
    lv_obj_align_to(lbl_l, slider_bal, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_text_color(lbl_l, lv_color_white(), 0);

    lv_obj_t * lbl_r = lv_label_create(lv_scr_act());
    lv_label_set_text(lbl_r, "R");
    lv_obj_align_to(lbl_r, slider_bal, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_set_style_text_color(lbl_r, lv_color_white(), 0);

    // Voice Interrupt Toggle
    lv_obj_t * label_int = lv_label_create(lv_scr_act());
    lv_label_set_text(label_int, "Interrupt");
    lv_obj_set_style_text_color(label_int, lv_color_white(), 0);
    lv_obj_align(label_int, LV_ALIGN_CENTER, -30, 65);

    lv_obj_t * sw_int = lv_switch_create(lv_scr_act());
    lv_obj_set_size(sw_int, 40, 20);
    lv_obj_align_to(sw_int, label_int, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    if (settings.enableInterrupt) lv_obj_add_state(sw_int, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_int, [](lv_event_t * e){
        if (static_dm) {
            settings.enableInterrupt = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Close Button
    lv_obj_t *btnClose = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnClose, 80, 40);
    lv_obj_align(btnClose, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_t *lblClose = lv_label_create(btnClose);
    lv_label_set_text(lblClose, "Close");
    lv_obj_center(lblClose);
    lv_obj_add_event_cb(btnClose, [](lv_event_t * e){
        settings.save(); // Save to NVRAM when menu is closed
        lv_async_call(async_show_main_ui, NULL);
    }, LV_EVENT_CLICKED, NULL);
}

static void showMicAecConfig() {
    g_isSubMenuActive = true;
    g_idleTimeout = 60000; // 60 seconds on sub-screens
    if (static_dm) static_dm->showWiFiError("");
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);
    boot_cont = nullptr;

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Mic Mode & AEC");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t * btnBack = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnBack, 50, 40);
    lv_obj_align(btnBack, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(btnBack, lv_color_make(60, 60, 60), 0);
    lv_obj_t * lblBack = lv_label_create(btnBack);
    lv_label_set_text(lblBack, LV_SYMBOL_LEFT);
    lv_obj_center(lblBack);
    lv_obj_add_event_cb(btnBack, [](lv_event_t * e){
        settings.save();
        lv_async_call(async_show_main_ui, NULL);
    }, LV_EVENT_CLICKED, NULL);

    // Scrollable container for the settings
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, 300, 170);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 5, 0);
    lv_obj_set_style_pad_gap(cont, 15, 0);

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // Auto-Tune Button
    lv_obj_t * btnTune = lv_btn_create(cont);
    lv_obj_set_size(btnTune, 270, 40);
    lv_obj_set_style_bg_color(btnTune, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t * lblTune = lv_label_create(btnTune);
    lv_label_set_text(lblTune, "Auto-Tune AEC");
    lv_obj_center(lblTune);
    lv_obj_add_event_cb(btnTune, [](lv_event_t * e){
        extern void tuneAEC();
        tuneAEC();
    }, LV_EVENT_CLICKED, NULL);

    // Mic Mode
    lv_obj_t * label_mic = lv_label_create(cont);
    lv_label_set_text(label_mic, "Microphone Mode");
    lv_obj_set_style_text_color(label_mic, lv_color_make(200, 200, 200), 0);
    lv_obj_t * dd_mic = lv_dropdown_create(cont);
    lv_dropdown_set_options(dd_mic, "Stereo (Beamforming)\nLeft Channel Only\nRight Channel Only");
    lv_obj_set_width(dd_mic, 270);
    lv_dropdown_set_selected(dd_mic, settings.micMode);
    lv_obj_add_event_cb(dd_mic, [](lv_event_t * e){
        settings.micMode = lv_dropdown_get_selected(lv_event_get_target(e));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Silence Threshold
    lv_obj_t * label_sil = lv_label_create(cont);
    lv_label_set_text(label_sil, "Silence Threshold");
    lv_obj_set_style_text_color(label_sil, lv_color_make(200, 200, 200), 0);
    lv_obj_t * ta_sil = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_sil, true);
    lv_textarea_set_accepted_chars(ta_sil, "0123456789");
    lv_textarea_set_text(ta_sil, String(settings.silenceThreshold).c_str());
    lv_obj_set_width(ta_sil, 270);
    lv_obj_add_event_cb(ta_sil, [](lv_event_t * e){
        lv_event_code_t code = lv_event_get_code(e);
        lv_obj_t * ta = lv_event_get_target(e);
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_t * cont = lv_obj_get_parent(ta);

        if (code == LV_EVENT_FOCUSED) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(kb);
            lv_obj_set_height(cont, 90); // Shrink container to fit above keyboard
            lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
            lv_obj_scroll_to_view(ta, LV_ANIM_ON);
        } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(cont, 170); // Restore container size
            lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -10);
            if (code == LV_EVENT_READY) lv_obj_clear_state(ta, LV_STATE_FOCUSED); // Drop focus on checkmark
            settings.silenceThreshold = String(lv_textarea_get_text(ta)).toInt();
        }
    }, LV_EVENT_ALL, kb);

    // AEC Cutoff
    lv_obj_t * label_cut = lv_label_create(cont);
    lv_label_set_text(label_cut, "AEC Cutoff");
    lv_obj_set_style_text_color(label_cut, lv_color_make(200, 200, 200), 0);
    lv_obj_t * ta_cut = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_cut, true);
    lv_textarea_set_accepted_chars(ta_cut, "0123456789");
    lv_textarea_set_text(ta_cut, String(settings.aecCutoff).c_str());
    lv_obj_set_width(ta_cut, 270);
    lv_obj_add_event_cb(ta_cut, [](lv_event_t * e){
        lv_event_code_t code = lv_event_get_code(e);
        lv_obj_t * ta = lv_event_get_target(e);
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_t * cont = lv_obj_get_parent(ta);

        if (code == LV_EVENT_FOCUSED) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(kb);
            lv_obj_set_height(cont, 90);
            lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
            lv_obj_scroll_to_view(ta, LV_ANIM_ON);
        } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(cont, 170);
            lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -10);
            if (code == LV_EVENT_READY) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
            int val = String(lv_textarea_get_text(ta)).toInt();
            settings.aecCutoff = val;
            extern void setAecCutoff(int cutoff);
            setAecCutoff(val);
        }
    }, LV_EVENT_ALL, kb);

    // AEC Attenuation
    lv_obj_t * label_att = lv_label_create(cont);
    lv_label_set_text(label_att, "AEC Attenuation Divisor");
    lv_obj_set_style_text_color(label_att, lv_color_make(200, 200, 200), 0);
    lv_obj_t * dd_att = lv_dropdown_create(cont);
    lv_dropdown_set_options(dd_att, "/1 (0%)\n/2 (50%)\n/4 (75%)\n/8 (87%)\n/16 (93%)\n/32 (96%)");
    lv_obj_set_width(dd_att, 270);
    int attIdx = 0;
    if (settings.aecAttenuation >= 32) attIdx = 5;
    else if (settings.aecAttenuation >= 16) attIdx = 4;
    else if (settings.aecAttenuation >= 8) attIdx = 3;
    else if (settings.aecAttenuation >= 4) attIdx = 2;
    else if (settings.aecAttenuation >= 2) attIdx = 1;
    lv_dropdown_set_selected(dd_att, attIdx);
    lv_obj_add_event_cb(dd_att, [](lv_event_t * e){
        int idx = lv_dropdown_get_selected(lv_event_get_target(e));
        int val = 1 << idx;
        settings.aecAttenuation = val;
        extern void setAecAttenuation(int atten);
        setAecAttenuation(val);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // AEC Delay
    lv_obj_t * label_dly = lv_label_create(cont);
    lv_label_set_text(label_dly, "AEC Delay (Samples)");
    lv_obj_set_style_text_color(label_dly, lv_color_make(200, 200, 200), 0);
    lv_obj_t * ta_dly = lv_textarea_create(cont);
    lv_textarea_set_one_line(ta_dly, true);
    lv_textarea_set_accepted_chars(ta_dly, "0123456789");
    lv_textarea_set_text(ta_dly, String(settings.aecDelay).c_str());
    lv_obj_set_width(ta_dly, 270);
    lv_obj_add_event_cb(ta_dly, [](lv_event_t * e){
        lv_event_code_t code = lv_event_get_code(e);
        lv_obj_t * ta = lv_event_get_target(e);
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_t * cont = lv_obj_get_parent(ta);

        if (code == LV_EVENT_FOCUSED) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(kb);
            lv_obj_set_height(cont, 90);
            lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
            lv_obj_scroll_to_view(ta, LV_ANIM_ON);
        } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(cont, 170);
            lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -10);
            if (code == LV_EVENT_READY) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
            int val = String(lv_textarea_get_text(ta)).toInt();
            settings.aecDelay = val;
            extern void setAecDelay(int delay);
            setAecDelay(val);
        }
    }, LV_EVENT_ALL, kb);
}

void DisplayManager::updateAudioVUMeter(int l, int r) {
    if (audio_vu_l) lv_bar_set_value(audio_vu_l, l, LV_ANIM_OFF);
    if (audio_vu_r) lv_bar_set_value(audio_vu_r, r, LV_ANIM_OFF);
}

void DisplayManager::setBalanceCallback(BalanceCallback cb) {
    balanceCb = cb;
}

void DisplayManager::balanceEventHandler(lv_event_t * e) {
    if (balanceCb) {
        lv_obj_t * slider = lv_event_get_target(e);
        balanceCb(lv_slider_get_value(slider));
    }
}

// --- Easter Egg ---
void renderBSOD() {
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }

    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(0, 0, 170), 0); // Classic BSOD Blue

    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 310);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label,
        "A problem has been detected and ESP32 has been shut down to prevent damage to your MCU.\n\n"
        "DRIVER_IRQL_NOT_LESS_OR_EQUAL\n\n"
        "If this is the first time you've seen this stop error screen, restart your device. If this screen appears again, follow these steps:\n\n"
        "Check to make sure any new hardware is properly installed and your wiring is correct.\n\n"
        "Technical information:\n\n"
        "*** STOP: 0x000000D1 (0x0000000C, 0x00000002, 0x00000000, 0xF86B5A89)\n"
        "***  ESP32_HAL.sys - Address F86B5A89 base at F86B5000\n\n"
        "Beginning dump of physical memory...");
    
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_timer_handler(); // Force the screen to draw instantly
}

void renderGuruMeditation() {
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }

    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    lv_obj_t * box = lv_obj_create(lv_scr_act());
    lv_obj_set_size(box, 300, 60);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_color(box, lv_color_make(255, 0, 0), 0);
    lv_obj_set_style_border_width(box, 4, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(box);
    lv_label_set_text(label, "Software Failure.  Press left mouse button to continue.\nGuru Meditation #00000004.0000AAC0");
    lv_obj_set_style_text_color(label, lv_color_make(255, 0, 0), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    
    lv_timer_handler(); // Force the screen to draw instantly
}

void forceClockScreen() {
    showClockScreen();
}

static lv_obj_t * aec_progress_bar = nullptr;
static lv_obj_t * aec_status_label = nullptr;

void showAecTuningUI() {
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }

    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Acoustic Echo Cancellation");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    aec_progress_bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(aec_progress_bar, 260, 20);
    lv_obj_align(aec_progress_bar, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(aec_progress_bar, 0, 100);
    lv_bar_set_value(aec_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(aec_progress_bar, lv_color_make(120, 120, 120), LV_PART_MAIN);
    lv_obj_set_style_bg_color(aec_progress_bar, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);

    aec_status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(aec_status_label, "Initializing test...");
    lv_obj_set_style_text_align(aec_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(aec_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(aec_status_label, lv_color_make(200, 200, 200), 0);
    lv_obj_align(aec_status_label, LV_ALIGN_CENTER, 0, 40);

    lv_timer_handler(); // Force the screen to draw instantly
}

void updateAecTuningUI(int percent, const char* msg) {
    if (aec_progress_bar) lv_bar_set_value(aec_progress_bar, percent, LV_ANIM_ON); // Animate the fill
    if (aec_status_label) {
        lv_label_set_text(aec_status_label, msg);
        lv_obj_align(aec_status_label, LV_ALIGN_CENTER, 0, 40); // Keep it perfectly centered
    }
    lv_timer_handler();
}

void showHelpScreen() {
    g_isSubMenuActive = false;
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }

    // Use public method to reset private pointers (statusLabel)
    if (static_dm) static_dm->showWiFiError("");
    
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);

    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Available Commands");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 255, 255), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t * list = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(list, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(list, 300);
    lv_obj_set_style_text_color(list, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
    lv_label_set_text(list,
        "- Volume [0-10]: Adjust audio level\n"
        "- Brightness [0-10]: Adjust screen backlight\n"
        "- Clock Color: Set to Red, Green, or White\n"
        "- Config Mode: Open Web UI\n"
        "- Calibrate Touchscreen: Fix touch alignment\n"
        "- Auto Tune: Calibrate Echo Cancellation\n"
        "- System Reboot: Restart device");
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 10, 45);

    lv_timer_handler(); // Force the screen to draw instantly
}