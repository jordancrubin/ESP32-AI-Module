# ESP32-S3 AI Voice Assistant

A fully integrated, voice-controlled AI assistant running on the ESP32-S3. This project transforms your microcontroller into a standalone smart home device by combining on-device wake-word detection, local command interception, Speech-to-Text, Large Language Model (LLM) intelligence, and Text-to-Speech via a beautiful touch interface.

![Device Banner](images/banner.jpg)

## Features

*   **Voice Activation**: On-device wake word detection using Edge Impulse (default: "Hey Espy").
*   **Conversational AI**: Integrates seamlessly with OpenAI-compatible APIs. Designed for local LLM servers (OpenWebUI, Ollama, LocalAI) but fully compatible with cloud providers.
    *   **Web Search & Memory**: Optional toggles to grant supported models internet access and persistent conversation context.
    *   **Knowledge/RAG**: Attach specific OpenWebUI Collection IDs to give the AI access to your local documents.
*   **Local Commands**: Intercepts specific spoken phrases (e.g., volume control, brightness, screen sleep) locally to execute instantly without API latency.
*   **Advanced Audio Pipeline**:
    *   **Input**: I2S Microphone support (ICS-43434) with software Acoustic Echo Cancellation (AEC) and Beamforming.
    *   **Output**: I2S Amplifier (MAX98357A) for high-quality Text-to-Speech playback.
    *   **Voice Barge-in**: Interrupt the AI while it's speaking by simply shouting over it! (Powered by SpeexDSP echo cancellation and dynamic amplitude gating).
*   **Interactive Display**:
    *   3.2" ILI9341 Touchscreen running LVGL with a clean, responsive dark theme.
    *   Hamburger dropdown menu for quick on-device access to Audio, AEC, and Voice settings.
    *   Graphical AEC Auto-Tuner with live visual progress bars.
    *   Real-time clock, live weather updates (OpenWeatherMap), and WiFi signal status.
*   **Web Configuration**: Comprehensive web interface for setting up WiFi, API keys, System Prompts, and Audio tuning.
*   **OTA Updates**: Update Firmware and Filesystem images wirelessly directly from your browser.

## Gallery

| Main Interface | Clock Mode | Web Config |
| :---: | :---: | :---: |
| ![Main UI](images/ui_main.jpg) | ![Clock](images/ui_clock.jpg) | ![Config](images/web_config.jpg) |

![Early development](images/early_dev.jpg)
<img src="images/early_dev.jpg" width="50%">
*Early development*

## Hardware Requirements

*   **MCU**: ESP32-S3 (DevKitC-1 or compatible) - **Must have PSRAM**.
*   **Display**: ILI9341 TFT Display (SPI) with XPT2046 Touch Controller.
*   **Audio Input**: ICS-43434 I2S Microphone (Stereo capable recommended for AEC).
*   **Audio Output**: MAX98357A I2S Amplifier + Speaker.
*   **Misc**: RGB LED (NeoPixel/WS2812B) for status indication.

## Pin Configuration

| Component | Pin (ESP32-S3) | Description |
| :--- | :--- | :--- |
| **Display** | | |
| TFT_CS | 10 | Chip Select |
| TFT_DC | 9 | Data/Command |
| TFT_RST | 46 | Reset |
| TFT_SCK | 12 | SPI Clock |
| TFT_MOSI | 11 | SPI MOSI |
| TFT_MISO | 13 | SPI MISO |
| TFT_BL | 4 | Backlight (PWM) |
| **Touch** | | |
| TOUCH_CS | 14 | Chip Select |
| TOUCH_IRQ | 255 | Interrupt (Disabled / Polling) |
| **Audio In** | | |
| I2S_SCK | 42 | BCLK |
| I2S_WS | 41 | LRCLK |
| I2S_SD | 40 | DIN |
| **Audio Out** | | |
| SPK_SCK | 7 | BCLK |
| SPK_WS | 6 | LRCLK |
| SPK_SD | 5 | DOUT |

## Software Architecture

The system operates in a continuous loop:
1.  **Listen**: The ESP32 listens for the wake word using a lightweight Edge Impulse model running on the DSP.
2.  **Record**: Upon trigger, audio is recorded to PSRAM. AEC is applied in real-time to cancel out any system audio (like music or previous speech).
3.  **Transcribe**: The recorded WAV is sent via HTTP POST to a Speech-to-Text (STT) endpoint (e.g., Whisper).
4.  **Process Local**: The transcribed text is checked against a list of local commands. If matched, the ESP32 executes the hardware action immediately.
5.  **Think**: If no local command is matched, the text is sent to an LLM endpoint (e.g., Llama 3.2).
6.  **Speak**: The AI's response is sent to a Text-to-Speech (TTS) endpoint (e.g., Kokoro) and played back via I2S.

## Setup & Installation

### 1. Firmware Upload
This project uses **PlatformIO**.
1.  Clone the repository.
2.  Open in VS Code with the PlatformIO extension.
3.  **Upload Filesystem Image**: This uploads `chime.mp3` and other assets to the LittleFS partition.
    *   *PlatformIO Task -> Platform -> Upload Filesystem Image*
4.  **Upload Firmware**: Build and flash the main application.
    *   *PlatformIO Task -> General -> Upload*

### 2. Backend Setup
The device requires an OpenAI-compatible API backend. The recommended local setup is **OpenWebUI** paired with Ollama and an audio pipeline:
*   **LLM**: Ollama running `llama3.2` or similar.
*   **Audio**: A container providing OpenAI-compatible `/v1/audio/transcriptions` and `/v1/audio/speech` endpoints (e.g., Whisper and Kokoro).

### 3. Initial Configuration
1.  Power on the device.
2.  If WiFi is not configured, the screen will prompt you to connect.
3.  Connect to the device's Access Point (if AP mode active) or find the IP in the Serial Monitor.
4.  Navigate to `http://aiesp.local` or the device IP address.
5.  **Default Login**: `admin` (Password is set on first boot via Serial or Touch UI).

### 4. Over-The-Air (OTA) Updates
The device supports wireless firmware and filesystem updates via the Web Interface.
1. Tap the Setup Gear icon on the display to enter Configuration Mode.
2. Navigate to `http://aiesp.local/update` in your browser.
3. Select your compiled `firmware.bin` or `littlefs.bin` file and upload.

## Configuration Guide

### Local Spoken Commands
The device intercepts specific phrases immediately after transcription to control hardware quickly without querying the LLM:
*   `"Set volume to [0-10]"` or `"Raise/lower the volume"`
*   `"Set brightness to [0-10]"` or `"Raise/lower the brightness"`
*   `"Change clock color to [red/green/white]"`
*   `"Go into configuration mode"`
*   `"Calibrate touchscreen"`
*   `"System reboot"`
*   `"Auto tune"` or `"Tune AEC"` (Runs the graphical AEC calibration)
*   `"Help"` (Displays and reads aloud available commands)
*   *Easter Eggs:* `"Show me your blue screen of death"` or `"Show me your guru meditation"`

### Web Interface
Access `http://aiesp.local` to configure:
*   **API Settings**: URL and Key for your LLM backend, plus TTS routing (OpenWebUI or Direct URL).
*   **System Prompt**: Define the personality of your assistant.
*   **Timezone**: Configure your local timezone for the idle clock screen.
*   **Weather**: OpenWeatherMap API key and location.
*   **Audio**: Microphone mode (Stereo/Left/Right), Silence Threshold, and Wake Word sensitivity.
*   **AEC & Interrupt**: Manually tweak the AEC Delay, Attenuation divisor, and Barge-in Cutoff.
*   **AI Features**: Enable Web Search, Conversation Memory, or OpenWebUI Knowledge Collection IDs.
*   **Data Collection**: Reboot into Edge Impulse USB Forwarder mode.
*   **Debug**: Enable verbose serial logging.

### Serial Commands
Connect via USB Serial (115200 baud) for advanced control:

| Command | Description |
| :--- | :--- |
| `/help` | List all available commands |
| `/settings` | Show current network, audio, and system settings |
| `/calibrate` | Start the touchscreen calibration utility |
| `/say <text>` | Force the device to speak specific text immediately |
| `/new` | Clear the current conversation history context |
| `/debug_aec` | Toggle Acoustic Echo Cancellation debug stats |
| `/test_mic` | Record a 5s clip and play it back (Debug mode only) |
| `/tune_aec` | Run the graphical AEC auto-tuner to lock in room acoustics |
| `/aec_delay <n>` | Set the AEC delay buffer alignment manually |
| `/aec_gain <n>` | Tune the AEC reference gain multiplier |
| `/aec_invert` | Toggle AEC phase inversion |

## Troubleshooting

**Touchscreen is inaccurate:**
Say `"Calibrate touchscreen"` or run `/calibrate` in the serial terminal.

**Wake word not triggering:**
1.  Check `Wake Thresh` in settings (lower is more sensitive).
2.  Ensure the microphone is connected correctly.
3.  Use `/test_mic` via the serial monitor to verify audio recording quality.

**Audio feedback/Echo during barge-in:**
1.  Ensure `Microphone Mode` is set to **Stereo** if using a stereo mic, as AEC requires a reference signal.
2.  Say `"Auto tune"` to run the on-device graphical acoustic calibration.

**"API Check Failed":**
Ensure your backend server is reachable from the ESP32's IP address. If using a local server, use the computer's IP (e.g., `192.168.1.x`), not `localhost`.

## License

MIT License. See `LICENSE` file for details.

## Credits

*   **Jordan Rubin** - Lead Developer
*   **Retro Tech & Electronics** - Project Inspiration & Hardware Design
*   **Edge Impulse** - Wake Word Model
*   **LVGL** - Graphics Library
*   **SpeexDSP** - Audio Processing

---
*2026 Jordan Rubin*