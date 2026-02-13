#pragma once
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <XPT2046_Touchscreen.h>
#include "Config.h"
#include "SettingsManager.h"

typedef void (*VolumeCallback)(int change);
typedef void (*BrightnessCallback)(int change);
typedef void (*WiFiConfigCallback)(String ssid, String pass);

class DisplayManager {
public:
    DisplayManager();
    void begin(TouchCalibration cal);
    void showMainUI();
    void showStatus(const char* message);
    void showResponse(const String& response);
    void showBootLogo();
    void showProgress(int percent, float speed);
    void showThinking(bool active);
    void setVolumeCallback(VolumeCallback cb);
    void setBrightnessCallback(BrightnessCallback cb);
    void setWiFiConfigCallback(WiFiConfigCallback cb);
    void showWiFiConfig();
    void showWiFiError(const char* message);
    void calibrateTouch(TouchCalibration& cal);
    void setBrightness(int level);
    bool getRawTouch(uint16_t *x, uint16_t *y);
    void clear();

private:
    static void volumeEventHandler(lv_event_t * e);
    static void brightnessEventHandler(lv_event_t * e);
    static void wifiConfigEventHandler(lv_event_t * e);
    static void wifiRetryHandler(lv_event_t * e);
    static VolumeCallback volumeCb;
    static BrightnessCallback brightnessCb;
    static WiFiConfigCallback wifiCb;
    Arduino_DataBus *bus;
    Arduino_GFX *gfx;
    XPT2046_Touchscreen *ts;
    lv_obj_t *statusLabel;
    lv_obj_t *spinner;
    lv_obj_t *wifi_dd;
    lv_obj_t *wifi_ta;
};