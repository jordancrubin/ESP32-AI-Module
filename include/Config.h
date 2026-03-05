#pragma once

// Pin Definitions
#define TFT_SCK    12
#define TFT_MOSI   11
#define TFT_MISO   13
#define TFT_CS     10
#define TFT_DC     9
#define TFT_RST    46

// Screen Resolution
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Touch Screen Pins (XPT2046)
#define TOUCH_CS   14
#define TOUCH_IRQ  255 // Set to 255 to disable interrupts and use polling

// I2S Microphone Pins (ICS-43434)
#define I2S_SCK    42 // BCLK
#define I2S_WS     41 // LRCL
#define I2S_SD     40 // DOUT

// I2S Speaker Pins (MAX98357)
#define SPEAKER_SCK 17 // BCLK
#define SPEAKER_WS  16 // LRC
#define SPEAKER_SD  15 // DIN

// Color Definitions
#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define WHITE 0xFFFF
#define BLACK 0x0000

// WiFi Credentials
extern const char* WIFI_SSID;
extern const char* WIFI_PASS;

// API Configuration
extern const char* OPENWEBUI_URL;
extern const char* OPENWEBUI_KEY;
extern const char* LLM_MODEL;

#define FIRMWARE_VERSION "0.0.2A"