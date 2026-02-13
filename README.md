# ESP32-S3 AI Voice Assistant

An integrated AI hardware module built on the ESP32-S3, featuring a touchscreen interface, voice recording, and real-time interaction with Large Language Models (LLMs) via Open WebUI or OpenAI-compatible APIs.

## 🚀 Features

- **Interactive Touch UI**: Powered by **LVGL v8.4**, featuring a responsive interface for volume, brightness, and WiFi configuration.
- **Voice Interaction**: 
  - **Speech Recording**: High-quality I2S audio capture using the `SpeechManager`.
  - **TTS Playback**: Streaming MP3 Text-to-Speech playback via `SpeakerManager` and `ESP8266Audio`.
- **LLM Integration**: Seamless connection to Open WebUI, Ollama, or OpenAI. Supports system prompts and conversation history with sliding window memory management to prevent OOM.
- **Smart Connectivity**: 
  - On-screen WiFi scanning and configuration.
  - mDNS support for resolving local network LLM servers (e.g., `http://chat.local`).
- **Robust Settings Management**: Persistent storage of WiFi credentials, API keys, volume, brightness, and touch calibration in NVS (Non-Volatile Storage).
- **Hardware Optimized**: Utilizes PSRAM for audio buffering and JSON processing to ensure smooth performance on the ESP32-S3.

## 🛠️ Hardware Requirements

- **Controller**: ESP32-S3 (N8R2 or higher recommended for PSRAM).
- **Display**: ILI9341 TFT LCD (320x240).
- **Touch**: XPT2046 Resistive Touch Controller.
- **Audio Input**: INMP441 or similar I2S Microphone.
- **Audio Output**: MAX98357A I2S DAC and Speaker.
- **Backlight**: PWM-controlled backlight on GPIO 4.

## 📂 Project Structure

- `src/main.cpp`: System orchestration and state machine.
- `src/DisplayManager.cpp`: LVGL initialization and UI screen management.
- `src/LLMClient.cpp`: API communication, JSON parsing, and TTS downloading.
- `src/SpeakerManager.cpp`: I2S audio playback and tone generation.
- `src/SpeechManager.cpp`: I2S microphone recording and WAV formatting.
- `src/SettingsManager.cpp`: NVS persistence for device configuration.

## ⚙️ Setup & Installation

1.  **PlatformIO**: This project is designed for use with the PlatformIO IDE.
2.  **Configuration**:
    - Create a `include/Secrets.h` file based on your environment:
      ```cpp
      #define SECRET_WIFI_SSID "Your_SSID"
      #define SECRET_WIFI_PASS "Your_Password"
      #define SECRET_OPENWEBUI_URL "http://your-server:3000/api/chat/completions"
      #define SECRET_OPENWEBUI_KEY "your_api_key"
      #define SECRET_LLM_MODEL "gpt-4o"
      ```
3.  **Build & Flash**:
    - Connect your ESP32-S3.
    - Run `pio run -t upload`.

## 🎮 Usage

### Touch Interface
- **Main Screen**: Displays AI responses and system status.
- **Controls**: Use the on-screen buttons to adjust volume and brightness.
- **WiFi Config**: If WiFi is not configured or connection fails, the module will automatically launch the WiFi setup utility.

### Serial Commands
You can interact with the module via the Serial Monitor (115200 baud):
- **Type a prompt**: Send any text to the LLM.
- **`/settings`**: View current system configuration, IP address, and signal strength.
- **`/config` / `/setup`**: Aliases for the settings dump.

## 🔧 Technical Details

### Audio Handling
The system uses a dedicated task pinned to Core 1 for audio playback to prevent "crackling" caused by WiFi or UI interrupts. It supports 16-bit PCM for tones and MP3 for LLM responses.

### Memory Management
To handle large JSON responses from LLMs, the project utilizes:
- **ArduinoJson** with filtering to extract only necessary data.
- **PSRAM** allocation for the request/response buffers and the TTS MP3 stream.

### Touch Calibration
On the first boot (or if settings are cleared), the module enters a calibration mode. Follow the on-screen prompts to calibrate the XPT2046 controller.

## 📜 License

This project is licensed under the MIT License - see the LICENSE file for details.

## 🙏 Acknowledgments
- LVGL for the graphics library.
- ESP8266Audio for the playback engine.
- Arduino_GFX for the display drivers.