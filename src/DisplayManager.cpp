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
static bool is_touch_active = false;
static bool touch_disabled = false;

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
}

void DisplayManager::begin(TouchCalibration cal) {
    _currentCal = cal;
    gfx->begin(20000000);
    gfx->setRotation(3);
    delay(100);

    // Initialize Backlight (Digital Mode)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

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

void DisplayManager::showMainUI(String currentVoice, int currentVolume, String voiceOptions) {
    _lastVoice = currentVoice;
    _lastVolume = currentVolume;
    if (voiceOptions.length() > 0) _voiceOptions = voiceOptions;

    lv_obj_clean(lv_scr_act());

    // Voice Dropdown
    lv_obj_t * dd_voice = lv_dropdown_create(lv_scr_act());
    lv_dropdown_set_options(dd_voice, _voiceOptions.c_str());
    lv_obj_set_width(dd_voice, 150);
    lv_obj_align(dd_voice, LV_ALIGN_TOP_LEFT, 5, 5);
    
    // Find and select current voice
    int index = 0;
    int currentIdx = 0;
    int start = 0;
    int end = _voiceOptions.indexOf('\n');
    while (end != -1 || start < _voiceOptions.length()) {
        String opt = (end == -1) ? _voiceOptions.substring(start) : _voiceOptions.substring(start, end);
        if (opt == currentVoice) {
            currentIdx = index;
            break;
        }
        index++;
        if (end == -1) break;
        start = end + 1;
        end = _voiceOptions.indexOf('\n', start);
    }
    lv_dropdown_set_selected(dd_voice, currentIdx);
    lv_obj_add_event_cb(dd_voice, voiceEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    // Volume Slider
    lv_obj_t * label_vol = lv_label_create(lv_scr_act());
    lv_label_set_text(label_vol, "Volume");
    lv_obj_align(label_vol, LV_ALIGN_TOP_LEFT, 5, 45);

    lv_obj_t * slider_vol = lv_slider_create(lv_scr_act());
    lv_obj_set_width(slider_vol, 110);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_LEFT, 5, 65);
    lv_slider_set_range(slider_vol, 0, 21);
    lv_slider_set_value(slider_vol, currentVolume, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_vol, volumeEventHandler, LV_EVENT_VALUE_CHANGED, NULL);

    // Create a basic label for status
    statusLabel = lv_label_create(lv_scr_act());
    lv_label_set_text(statusLabel, "Initializing...");
    lv_obj_align(statusLabel, LV_ALIGN_TOP_RIGHT, -5, 50);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_width(statusLabel, SCREEN_WIDTH - 130);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_LEFT, 0);

    // Setup Button (Top Right)
    lv_obj_t *btnSetup = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btnSetup, 60, 35);
    lv_obj_align(btnSetup, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_t *lblSetup = lv_label_create(btnSetup);
    lv_label_set_text(lblSetup, "Setup");
    lv_obj_center(lblSetup);
    lv_obj_add_event_cb(btnSetup, setupEventHandler, LV_EVENT_CLICKED, this);

    // Create Thinking Spinner (Top Right)
    spinner = lv_spinner_create(lv_scr_act(), 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_RIGHT, -70, 5); // Moved left of Setup button
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -20);

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