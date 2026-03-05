#pragma once
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <XPT2046_Touchscreen.h>
#include "Config.h"
#include "SettingsManager.h"

typedef void (*VolumeCallback)(int value);
typedef void (*WiFiConfigCallback)(String ssid, String pass);
typedef void (*APIConfigCallback)(String key);
typedef void (*APIUrlConfigCallback)(String url);
typedef void (*AdminConfigCallback)(String pass);
typedef void (*VoiceCallback)(String voice);
typedef void (*SetupModeCallback)(bool enabled);

class DisplayManager {
public:
    DisplayManager();
    void begin(TouchCalibration cal);
    void showMainUI(String currentVoice = "alloy", int currentVolume = 21, String voiceOptions = "");
    void showStatus(const char* message);
    void showResponse(const String& response);
    void showBootLogo();
    void showProgress(int percent, float speed);
    void showThinking(bool active);
    void setVolumeCallback(VolumeCallback cb);
    void setWiFiConfigCallback(WiFiConfigCallback cb);
    void showWiFiConfig();
    void showWiFiError(const char* message);
    void setAPIConfigCallback(APIConfigCallback cb);
    void showAPIConfig(String currentKey = "");
    void setAPIUrlConfigCallback(APIUrlConfigCallback cb);
    void showAPIUrlConfig(String currentUrl = "");
    void setAdminConfigCallback(AdminConfigCallback cb);
    void showAdminConfig();
    void showWebConfig(String ip, String hostname);
    void setVoiceCallback(VoiceCallback cb);
    void setSetupModeCallback(SetupModeCallback cb);
    void calibrateTouch(TouchCalibration& cal);
    bool getRawTouch(uint16_t *x, uint16_t *y);
    void clear();
    void fadeBacklight(uint8_t target, int durationMs);
    void setBacklight(uint8_t brightness);
    void updateWeather(const char* temp, const char* desc);
    char _weatherTemp[16];
    char _weatherDesc[32];

private:
    static void volumeEventHandler(lv_event_t * e);
    static void wifiConfigEventHandler(lv_event_t * e);
    static void wifiRetryHandler(lv_event_t * e);
    static void apiConfigEventHandler(lv_event_t * e);
    static void apiUrlConfigEventHandler(lv_event_t * e);
    static void adminConfigEventHandler(lv_event_t * e);
    static void setupEventHandler(lv_event_t * e);
    static void closeSetupEventHandler(lv_event_t * e);
    static void voiceEventHandler(lv_event_t * e);
    static VolumeCallback volumeCb;
    static WiFiConfigCallback wifiCb;
    static APIConfigCallback apiCb;
    static APIUrlConfigCallback apiUrlCb;
    static AdminConfigCallback adminCb;
    static VoiceCallback voiceCb;
    static SetupModeCallback setupModeCb;
    Arduino_DataBus *bus;
    Arduino_GFX *gfx;
    XPT2046_Touchscreen *ts;
    lv_obj_t *statusLabel;
    lv_obj_t *spinner;
    lv_obj_t *wifi_dd;
    lv_obj_t *wifi_ta;
    String _lastVoice;
    int _lastVolume;
    String _voiceOptions;
    uint8_t _currentBrightness;
};