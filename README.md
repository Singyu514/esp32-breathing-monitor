# Real-Time AI Breathing & Motion Monitor on ESP32-S3

**Course:** Computer-Aided Medical Diagnostics (MPA-PRM)  
**Lab:** Application IV: Real-time AI applications  
**Institution:** Brno University of Technology (BUT)  

---

## 👥 Team Members
- **Sing-Yu Lin**
- **Maxime Brière**

- **Project Repository:** [https://github.com/Singyu514/esp32-breathing-monitor](https://github.com/Singyu514/esp32-breathing-monitor)
- **Live BLE Monitoring Station (GitHub Pages):** [https://singyu514.github.io/esp32-breathing-monitor/](https://singyu514.github.io/esp32-breathing-monitor/) *(Deployment in progress)*

---

## 🎯 Project Overview
This project builds a real-time patient monitoring device on an **ESP32-S3 N16R8** development board connected to an **MPU-6050 (GY-521)** 6-DOF IMU (accelerometer & gyroscope).

The device autonomously classifies 4 distinct physical and respiratory states directly on the microcontroller without sending data to an external computer for inference:

| State | Physical Condition | Board RGB LED |
| :--- | :--- | :--- |
| **Calm breathing** | Sensor placed against chest, regular resting respiration | 🟢 **Green** |
| **Fast breathing** | Sensor placed against chest, rapid / tachypneic respiration | 🔴 **Red** |
| **Not attached** | Device held in hand, moved slowly, or resting on a table | 🔵 **Blue** |
| **Shaking** | Device being shaken vigorously | 🟣 **Magenta** |

---

## 🔌 Hardware Setup & Wiring

### Components:
- **Microcontroller:** ESP32-S3 N16R8 DevKit
- **Sensor:** MPU-6050 (GY-521) 3-axis Accelerometer + 3-axis Gyroscope
- **Indicator:** On-board addressable RGB LED (WS2812 / NeoPixel)
- **Connections:** Breadboard & jumper wires, powered via USB cable

### Pinout (I2C):
| MPU-6050 Pin | ESP32-S3 Pin | Description |
| :--- | :--- | :--- |
| **VCC** | `3V3` / `5V` | Power supply |
| **GND** | `GND` | Ground |
| **SCL** | `GPIO 9` *(default)* | I2C Clock |
| **SDA** | `GPIO 8` *(default)* | I2C Data |
| **AD0** | `GND` | I2C address selection (`0x68`) |

> *Photo of the wiring will be placed here: `docs/wiring.jpg`.*

---

## 📊 Project Progress & Grading Milestones

- [x] **Repository & Project Setup** (Compulsory)
  - [x] Git repository created on branch `main`
  - [x] README and structure initialized
  - [ ] Shared with teacher (`tomasvicar`)
- [x] **Part 1: Movement vs. Shaking (2.5 pts)** - *Validated by teacher*
  - [x] Omnidirectional sensor reading over I2C (SDA: 8, SCL: 9) at 50 Hz
  - [x] Rolling window differential energy metric (`d_acc + 0.5 * d_gyr`)
  - [x] Real-time RGB LED feedback (Blue = Slow movement / Still, Magenta = Shaking)
- [x] **Part 2: Breathing Classification (2.5 pts)** - *Validated on board (98% accuracy)*
  - [x] Data collection across 16 sessions (calm, fast, lying still, moving in hand, shaking)
  - [x] Feature extraction: 6 spatial & dynamic IMU metrics with rotation awareness
  - [x] Embedded C++ Random Forest inference (8 trees, depth 5) + rolling majority smoothing filter
  - [x] Real-time RGB LED feedback: Green (calm), Red (fast), Blue (not attached), Magenta (shaking)
- [x] **Part 3: BLE Bedside Monitor (1.5 pts)** - *Ready & deployed*
  - [x] BLE GATT Service & State Characteristic notification on ESP32-S3 (`ESP32_Breathing_Monitor`)
  - [x] Web Bluetooth monitoring dashboard on GitHub Pages (full English clinical UI)
  - [x] Real-time state timeline visualization
  - [x] Configurable duration alarms (Tachypnea > 8s, Detached > 10s) with audio beeper
  - [x] Instantaneous connection-lost alarm with critical siren
- [x] **Part 4: Web Page from the Board (1.0 pt)** - *Ready to show*
  - [x] ESP32-S3 Wi-Fi SoftAP hotspot (`ESP32_Breathing_Monitor`)
  - [x] Standalone HTTP web server running on chip (`http://192.168.4.1`)
  - [x] Live acceleration signal waveform oscilloscope (pure Canvas, zero external dependencies)
  - [x] Real-time prediction state badge and all 4 class probability confidence bars
- [ ] **Part 5: Own Extension (1.5 pts)**
  - [ ] Definition & implementation of custom enhancement (e.g., dynamic breathing rate estimation / posture & apnea alerts / fallback uncertainty detection)
- [ ] **Part 6: Documentation & Reproducibility (1.0 pt)**
  - [ ] Full build and flashing instructions
  - [ ] Model architecture, hyperparameter documentation, and performance report
  - [ ] AI tools declaration

---

## 🧠 Machine Learning Pipeline
1. **Data Acquisition:** Sampling accelerometer & gyroscope axes at ~50 Hz.
2. **Windowing & Preprocessing:** Sliding window (e.g. 2–3 seconds with 50% overlap). Log transform on amplitude features.
3. **Feature Extraction:** Mean, variance, peak-to-peak amplitude, spectral energy / dominant frequency.
4. **Classifier:** Lightweight MLP / Random Forest / Decision Tree exported to efficient C++ code (`emlearn` or embedded tensor inference).
5. **Decision Smoothing:** Rolling median / majority vote filter over successive windows to eliminate spurious transitions.

---

## 🤖 AI Tools Declaration
In compliance with Brno University of Technology (BUT) guidelines:
- **AI Tool Used:** Google Antigravity / Gemini 3.8 assistant.
- **Scope of Use:** Initial project scaffolding, documentation structure, assistance with firmware skeleton (I2C/BLE/Wi-Fi), signal processing and feature engineering recipes, and Web Bluetooth API boilerplate. All code, data collection, and physical validation on the board are understood, checked, and run by the team members.

---

## 🚀 How to Run & Rebuild

### 1. Requirements
- Arduino IDE 2.x or PlatformIO
- Python 3.10+ (for model training: `numpy`, `scipy`, `scikit-learn`, `matplotlib`, `pandas`)
- Chrome / Edge with Web Bluetooth enabled (for BLE bedside monitor)

### 2. Microcontroller Firmware
1. Open the project firmware in Arduino IDE or VS Code PlatformIO.
2. Select board **ESP32S3 Dev Module** with appropriate partition scheme and USB CDC on boot enabled.
3. Flash the firmware onto the ESP32-S3.
4. Unplug and replug or power via standalone USB power supply.
