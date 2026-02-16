# ESP32 AI Module Firmware

An open-source AI voice assistant firmware designed for the ESP32-S3, featuring a modern touchscreen interface, voice interaction, and integration with OpenAI-compatible LLM APIs.

## Features

*   **Voice Interaction:** Record speech via I2S microphone and process it with an external LLM.
*   **Text-to-Speech (TTS):** Plays back AI responses using high-quality MP3 streaming.
*   **Touchscreen UI:** Built with LVGL, featuring a responsive interface for settings, volume control, and status monitoring.
*   **Clock Mode:** A stylish 7-segment clock display with WiFi signal strength and day of week indicators that activates when idle.
*   **Web Configuration:** Configure WiFi credentials, API keys, Timezone, and other settings via a web portal.
*   **Customizable:** Change AI voices, clock colors, and LLM models directly from the device or web interface.

## Hardware Requirements

*   **Microcontroller:** ESP32-S3 (Tested on ESP32-S3-DevKitC-1-N8R2 with 8MB Flash / 2MB PSRAM).
*   **Display:** ILI9341 TFT LCD (320x240) with XPT2046 Touch Controller.
*   **Audio Input:** I2S Microphone (e.g., INMP441).
*   **Audio Output:** I2S Amplifier (e.g., MAX98357A) + Speaker.

## Software Requirements

*   **PlatformIO:** This project is built using PlatformIO.
*   **Framework:** Arduino for ESP32.
*   **Libraries:**
    *   LVGL (Light and Versatile Graphics Library)
    *   TFT_eSPI or Arduino_GFX
    *   ArduinoJson
    *   ESP32-audioI2S
    *   TJpg_Decoder

## Installation

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/yourusername/ESP32-AI-Module.git
    ```
2.  **Open in PlatformIO:** Open the project folder in VS Code with the PlatformIO extension installed.
3.  **Build and Upload:** Connect your ESP32-S3 via USB and click the "Upload" button.
4.  **Filesystem:** Ensure you upload the filesystem image if you have custom assets (Boot logo, etc.).

## Configuration

### Initial Setup
1.  **WiFi:** On first boot, if no WiFi is configured, the device will prompt you to connect via the touchscreen.
2.  **Calibration:** If the touch screen is not calibrated, a calibration routine will run automatically. Follow the on-screen dots.

### Web Interface
The device hosts a web server for easy configuration.
1.  Connect the device to WiFi.
2.  Navigate to `http://aiesp.local` or the device's IP address in a web browser.
3.  **Login:** Use the admin password (set on the device).
4.  **Settings Available:**
    *   **API Key:** Your LLM provider's API Key.
    *   **API URL:** The endpoint for the Chat API (e.g., `http://your-server:8080/api/chat/completions`).
    *   **Timezone:** Select your local timezone for the clock.
    *   **Clock Color:** Choose between Red, Green, or White.

## Usage

*   **Idle Mode:** The screen turns off or switches to a digital clock after 30 seconds of inactivity. Tap the screen to wake.
*   **Voice Commands:** The device records audio and sends it to the configured LLM endpoint.
*   **Settings:** Tap the "Setup" (Gear icon) button on the main screen to view IP address, RSSI, and access web config mode.

## Credits

Designed and developed by Jordan Rubin.

*   **YouTube:** RetroTech & Electronics
*   **Copyright:** 2026 Jordan Rubin

---