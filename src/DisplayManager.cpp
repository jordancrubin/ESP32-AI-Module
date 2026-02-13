#include "DisplayManager.h"
#include <Fonts/FreeSans12pt7b.h>
#include <TJpg_Decoder.h>
#include "BootLogo.h"
#include <WiFi.h>

// GPIO 38 conflicts with the PSRAM bus on S3 modules, causing audio distortion.
// GPIO 4 is a safe pin for PWM backlight control.
#define TFT_BL 4

// Static reference for the callback
static Arduino_GFX *static_gfx = nullptr;
static DisplayManager *static_dm = nullptr;
static TouchCalibration _currentCal;

// LVGL Touchpad Read Callback
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
    if (!static_dm) return;

    uint16_t touchX, touchY;
    if (static_dm->getRawTouch(&touchX, &touchY)) {
        data->state = LV_INDEV_STATE_PR;

        // Use saved calibration if valid, otherwise fallback to defaults
        uint16_t xMin = _currentCal.isValid ? _currentCal.xMin : 200;
        uint16_t xMax = _currentCal.isValid ? _currentCal.xMax : 3800;
        uint16_t yMin = _currentCal.isValid ? _currentCal.yMin : 200;
        uint16_t yMax = _currentCal.isValid ? _currentCal.yMax : 3800;

        data->point.x = map(touchX, xMin, xMax, 0, SCREEN_WIDTH);
        data->point.y = map(touchY, yMin, yMax, 0, SCREEN_HEIGHT);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// Callback function for TJpg_Decoder
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (static_gfx) {
        static_gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
    }
    return true;
}

VolumeCallback DisplayManager::volumeCb = nullptr;
BrightnessCallback DisplayManager::brightnessCb = nullptr;
WiFiConfigCallback DisplayManager::wifiCb = nullptr;

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
}

void DisplayManager::begin(TouchCalibration cal) {
    _currentCal = cal;
    gfx->begin(20000000);
    gfx->setRotation(3);
    delay(100);

    // Initialize Backlight PWM
    // Lowering frequency to 200Hz reduces electrical noise (EMI) that causes audio distortion.
    ledcAttach(TFT_BL, 200, 8);
    ledcWrite(TFT_BL, 255); // Default to full brightness

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

    showMainUI();
}

void DisplayManager::showMainUI() {
    lv_obj_clean(lv_scr_act());

    // Create a basic label for status
    statusLabel = lv_label_create(lv_scr_act());
    lv_label_set_text(statusLabel, "Initializing...");
    lv_obj_align(statusLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_width(statusLabel, SCREEN_WIDTH - 20);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);

    // Create Thinking Spinner (Top Right)
    spinner = lv_spinner_create(lv_scr_act(), 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_t * sp_label = lv_label_create(spinner);
    lv_label_set_text(sp_label, "?");
    lv_obj_center(sp_label);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    // Volume Buttons
    lv_obj_t *btnMinus = lv_btn_create(lv_scr_act());
    lv_obj_align(btnMinus, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_size(btnMinus, 70, 70);
    lv_obj_t *lblMinus = lv_label_create(btnMinus);
    lv_label_set_text(lblMinus, "-");
    lv_obj_set_style_text_font(lblMinus, &lv_font_montserrat_14, 0);
    lv_obj_center(lblMinus);
    lv_obj_add_event_cb(btnMinus, volumeEventHandler, LV_EVENT_CLICKED, (void*)-1);

    lv_obj_t *btnPlus = lv_btn_create(lv_scr_act());
    lv_obj_align(btnPlus, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_size(btnPlus, 70, 70);
    lv_obj_t *lblPlus = lv_label_create(btnPlus);
    lv_label_set_text(lblPlus, "+");
    lv_obj_set_style_text_font(lblPlus, &lv_font_montserrat_14, 0);
    lv_obj_center(lblPlus);
    lv_obj_add_event_cb(btnPlus, volumeEventHandler, LV_EVENT_CLICKED, (void*)1);

    // Brightness Buttons
    lv_obj_t *btnBriMinus = lv_btn_create(lv_scr_act());
    lv_obj_align(btnBriMinus, LV_ALIGN_BOTTOM_LEFT, 10, -90);
    lv_obj_set_size(btnBriMinus, 70, 70);
    lv_obj_t *lblBriMinus = lv_label_create(btnBriMinus);
    lv_label_set_text(lblBriMinus, "B-");
    lv_obj_center(lblBriMinus);
    lv_obj_add_event_cb(btnBriMinus, brightnessEventHandler, LV_EVENT_CLICKED, (void*)-15);

    lv_obj_t *btnBriPlus = lv_btn_create(lv_scr_act());
    lv_obj_align(btnBriPlus, LV_ALIGN_BOTTOM_RIGHT, -10, -90);
    lv_obj_set_size(btnBriPlus, 70, 70);
    lv_obj_t *lblBriPlus = lv_label_create(btnBriPlus);
    lv_label_set_text(lblBriPlus, "B+");
    lv_obj_center(lblBriPlus);
    lv_obj_add_event_cb(btnBriPlus, brightnessEventHandler, LV_EVENT_CLICKED, (void*)15);
}

void DisplayManager::clear() {
    // LVGL handles background clearing automatically
    if (statusLabel) lv_label_set_text(statusLabel, "");
}

void DisplayManager::showStatus(const char* message) {
    if (statusLabel) {
        lv_label_set_text(statusLabel, message);
    }
}

void DisplayManager::showResponse(const String& response) {
    if (statusLabel) {
        lv_label_set_text(statusLabel, response.c_str());
    }
}

void DisplayManager::showBootLogo() {
    gfx->fillScreen(BLACK);
    
    // Configure TJpg_Decoder
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true); // Swap bytes for SPI TFT
    TJpgDec.setCallback(tft_output);

    // Center the image
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, boot_logo, sizeof(boot_logo));
    
    Serial.printf("Boot Logo Size: %dx%d\n", w, h);

    int x = (gfx->width() - w) / 2;
    int y = (gfx->height() - h) / 2;
    
    TJpgDec.drawJpg(x, y, boot_logo, sizeof(boot_logo));
}

void DisplayManager::showProgress(int percent, float speed) {
    if (statusLabel) {
        String msg = "Downloading Audio...\n" + String(percent) + "%\n" + String(speed, 1) + " KB/s";
        lv_label_set_text(statusLabel, msg.c_str());
    }
}

void DisplayManager::showThinking(bool active) {
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
    
    lv_obj_t * label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "WiFi Configuration");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    wifi_dd = lv_dropdown_create(lv_scr_act());
    lv_obj_set_width(wifi_dd, 200);
    lv_obj_align(wifi_dd, LV_ALIGN_TOP_MID, 0, 40);
    lv_dropdown_set_text(wifi_dd, "Scanning...");

    wifi_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, true);
    lv_textarea_set_placeholder_text(wifi_ta, "Password");
    lv_obj_set_width(wifi_ta, 200);
    lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t * kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, wifi_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(wifi_ta, [](lv_event_t* e){
        lv_obj_t* kb = (lv_obj_t*)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, kb);

    lv_obj_t * btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 100, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_t * btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Connect");
    lv_obj_add_event_cb(btn, wifiConfigEventHandler, LV_EVENT_CLICKED, this);

    int n = WiFi.scanNetworks();
    String opts = "";
    for (int i = 0; i < n; i++) {
        opts += WiFi.SSID(i);
        if (i < n - 1) opts += "\n";
    }
    lv_dropdown_set_options(wifi_dd, opts.c_str());
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

void DisplayManager::wifiRetryHandler(lv_event_t * e) {
    static_dm->showWiFiConfig();
}

void DisplayManager::setBrightness(int level) {
    ledcWrite(TFT_BL, constrain(level, 0, 255));
}

void DisplayManager::setVolumeCallback(VolumeCallback cb) {
    volumeCb = cb;
}

void DisplayManager::setBrightnessCallback(BrightnessCallback cb) {
    brightnessCb = cb;
}

void DisplayManager::volumeEventHandler(lv_event_t * e) {
    if (volumeCb) {
        int change = (int)(intptr_t)lv_event_get_user_data(e);
        volumeCb(change);
    }
}

void DisplayManager::brightnessEventHandler(lv_event_t * e) {
    if (brightnessCb) {
        int change = (int)(intptr_t)lv_event_get_user_data(e);
        brightnessCb(change);
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
        
        // Wait for touch with timeout (10s)
        unsigned long start = millis();
        unsigned long lastBeat = 0;
        while(millis() - start < 10000) {
            if (getRawTouch(&rawX, &rawY)) {
                gfx->fillCircle(x, y, 5, GREEN);
                delay(500); // Debounce
                return true;
            }
            if (millis() - lastBeat > 1000) { Serial.print("."); lastBeat = millis(); }
            delay(10);
        }
        return false;
    };

    uint16_t x1, y1, x2, y2;
    
    // Point 1: Top Left
    if (!waitForTouch(20, 20, x1, y1)) {
        gfx->setFont(&FreeSans12pt7b);
        Serial.println("Calibration timeout, using defaults");
        cal = {200, 3800, 200, 3800, true}; // Set defaults
        return;
    }
    
    gfx->fillScreen(BLACK);
    gfx->setFont(NULL);
    delay(500);
    
    // Point 2: Bottom Right
    if (!waitForTouch(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20, x2, y2)) {
         gfx->setFont(&FreeSans12pt7b);
         Serial.println("Calibration timeout, using defaults");
         cal = {200, 3800, 200, 3800, true}; // Set defaults
         return;
    }

    cal.xMin = x1;
    cal.yMin = y1;
    cal.xMax = x2;
    cal.yMax = y2;
    cal.isValid = true;
    
    // Update the internal static copy so the LVGL callback uses new values immediately
    _currentCal = cal;

    gfx->setFont(&FreeSans12pt7b);
    gfx->fillScreen(BLACK);
    gfx->setCursor(20, SCREEN_HEIGHT/2);
    gfx->print("Calibration Saved!");
    delay(1000);
}