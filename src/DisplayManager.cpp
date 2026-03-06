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

// Global state for Clock/Idle handling
static String g_lastVoice = "alloy";
static int g_lastVolume = 21;
static String g_voiceOptions = "alloy";
static String g_lastStatus = "Ready";
static lv_timer_t * g_clockTimer = nullptr;
static lv_timer_t * g_idleTimer = nullptr;
static void showVoiceModelConfig();
static void setupBootScreen();
static lv_obj_t * boot_cont = nullptr;
static VoiceCallback g_voiceCb = nullptr;
static uint16_t * s_boot_buffer = nullptr;
static uint16_t s_boot_w = 0;
static uint16_t s_boot_h = 0;

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
    int currentDay = timeinfo.tm_wday; // 0=Sun, 1=Mon...
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
    // Restore Main UI
    if (static_dm) {
        static_dm->showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
    }
}

// Callback to check for inactivity
static void idle_timer_cb(lv_timer_t * t) {
    // If inactive for 30 seconds, switch to clock
    if (lv_disp_get_inactive_time(NULL) > 30000) {
        if (g_idleTimer) {
             lv_timer_del(g_idleTimer);
             g_idleTimer = nullptr;
        }
        showClockScreen();
    }
}

static void showClockScreen() {
    lv_obj_clean(lv_scr_act());
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
    lv_obj_clean(lv_scr_act());
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
        if (static_dm) static_dm->showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
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
    if (g_clockTimer) {
        lv_timer_del(g_clockTimer);
        g_clockTimer = nullptr;
    }

    _lastVoice = currentVoice;
    _lastVolume = currentVolume;
    if (voiceOptions.length() > 0) _voiceOptions = voiceOptions;
    
    // Update globals for restoration from clock
    g_lastVoice = _lastVoice;
    g_lastVolume = _lastVolume;
    g_voiceOptions = _voiceOptions;

    boot_cont = nullptr; // Clear boot container reference
    lv_obj_clean(lv_scr_act());
    // Dark Theme Background
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(20, 20, 20), 0);

    // --- Header / Top Right ---

    // Setup Button (Icon)
    lv_obj_t *btnSetup = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnSetup, 40, 40);
    lv_obj_align(btnSetup, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btnSetup, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_radius(btnSetup, 20, 0); // Circle
    lv_obj_t *lblSetup = lv_label_create(btnSetup);
    lv_label_set_text(lblSetup, LV_SYMBOL_SETTINGS);
    lv_obj_center(lblSetup);
    lv_obj_add_event_cb(btnSetup, setupEventHandler, LV_EVENT_CLICKED, this);

    // Config Button (Left of Setup)
    lv_obj_t *btnConfig = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnConfig, 40, 40);
    lv_obj_align_to(btnConfig, btnSetup, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_color(btnConfig, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_radius(btnConfig, 20, 0);
    lv_obj_t *lblConfig = lv_label_create(btnConfig);
    lv_label_set_text(lblConfig, LV_SYMBOL_LIST);
    lv_obj_center(lblConfig);
    lv_obj_add_event_cb(btnConfig, [](lv_event_t * e){
        showVoiceModelConfig();
    }, LV_EVENT_CLICKED, NULL);

    // --- Volume Area (Top Left) ---

    // Volume Label
    lv_obj_t * label_vol = lv_label_create(lv_scr_act());
    lv_label_set_text(label_vol, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(label_vol, lv_color_make(200, 200, 200), 0);
    lv_obj_align(label_vol, LV_ALIGN_TOP_LEFT, 10, 20);

    // Volume Slider (Horizontal)
    lv_obj_t * slider_vol = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider_vol, 140);
    lv_obj_set_height(slider_vol, 10);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_LEFT, 40, 23);
    lv_slider_set_range(slider_vol, 0, 21);
    lv_slider_set_value(slider_vol, currentVolume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_vol, lv_color_make(60, 60, 60), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_vol, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider_vol, volumeEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    // --- Status Area (Center/Bottom) ---

    // Status Container (Visual background for text, no interaction)
    lv_obj_t * statusCont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(statusCont, 300, 160);
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
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    
    // Start/Restart Idle Timer
    if (!g_idleTimer) {
        g_idleTimer = lv_timer_create(idle_timer_cb, 1000, NULL);
    }
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
    }
}

void DisplayManager::showResponse(const String& response) {
    g_lastStatus = response;
    if (g_clockTimer) {
        showMainUI(g_lastVoice, g_lastVolume, g_voiceOptions);
    }
    if (statusLabel) {
        lv_label_set_text(statusLabel, response.c_str());
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
            Serial.printf("Boot Logo Size: %dx%d\n", w, h);
            int x = (gfx->width() - w) / 2;
            int y = (gfx->height() - h) / 2;
            TJpgDec.drawJpg(x, y, boot_logo, sizeof(boot_logo));
        } else {
            Serial.println("Boot Logo Error: Invalid JPG data.");
        }
    } else {
        Serial.println("Boot Logo Error: Unknown format.");
    }
}

void DisplayManager::showThinking(bool active) {
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
    lv_obj_clean(lv_scr_act());
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
    lv_obj_clean(lv_scr_act());
    
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
    lv_obj_clean(lv_scr_act());
    
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
    lv_obj_clean(lv_scr_act());
    
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
    if (g_idleTimer) {
        lv_timer_del(g_idleTimer);
        g_idleTimer = nullptr;
    }

    lv_obj_clean(lv_scr_act());
    
    lv_obj_t * title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "Configuration Required");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * instr = lv_label_create(lv_scr_act());
    lv_label_set_text(instr, "Connect to:");
    lv_obj_align(instr, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * url_label = lv_label_create(lv_scr_act());
    String url = "http://" + hostname;
    lv_label_set_text(url_label, url.c_str());
    lv_obj_set_style_text_font(url_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(url_label, lv_color_make(100, 200, 255), 0);
    lv_obj_align(url_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t * ip_label = lv_label_create(lv_scr_act());
    String ipStr = "(" + ip + ")";
    lv_label_set_text(ip_label, ipStr.c_str());
    lv_obj_align(ip_label, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t * note = lv_label_create(lv_scr_act());
    lv_label_set_text(note, "Use Admin Password to login");
    lv_obj_set_style_text_color(note, lv_color_make(180, 180, 180), 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -60);

    // Debug Toggle
    lv_obj_t * sw = lv_switch_create(lv_scr_act());
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -20, 55);
    if (settings.debugMode) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, [](lv_event_t * e){
        lv_obj_t * obj = lv_event_get_target(e);
        settings.debugMode = lv_obj_has_state(obj, LV_STATE_CHECKED);
        settings.save();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * sw_lbl = lv_label_create(lv_scr_act());
    lv_label_set_text(sw_lbl, "Debug");
    lv_obj_align_to(sw_lbl, sw, LV_ALIGN_OUT_TOP_MID, 0, -5);

    lv_obj_t * baud_lbl = lv_label_create(lv_scr_act());
    lv_label_set_text(baud_lbl, "Serial Baud: 115200");
    lv_obj_set_style_text_font(baud_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(baud_lbl, lv_color_make(150, 150, 150), 0);
    lv_obj_align_to(baud_lbl, sw, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    // Close Button
    lv_obj_t *btnClose = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnClose, 80, 40);
    lv_obj_align(btnClose, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_t *lblClose = lv_label_create(btnClose);
    lv_label_set_text(lblClose, "Close");
    lv_obj_center(lblClose);
    lv_obj_add_event_cb(btnClose, closeSetupEventHandler, LV_EVENT_CLICKED, this);
}

void DisplayManager::showWiFiError(const char* message) {
    lv_obj_clean(lv_scr_act());
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
        Serial.println("Setup button pressed. Web Server is active.");
        if (setupModeCb) setupModeCb(true);
        dm->showWebConfig(WiFi.localIP().toString(), "aiesp.local");
    }
}

void DisplayManager::closeSetupEventHandler(lv_event_t * e) {
    DisplayManager* dm = (DisplayManager*)lv_event_get_user_data(e);
    if (dm) {
        if (setupModeCb) setupModeCb(false);
        dm->showMainUI(dm->_lastVoice, dm->_lastVolume);
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
    static_dm->showWiFiConfig();
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
                Serial.println("\nCalibration aborted via Serial.");
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