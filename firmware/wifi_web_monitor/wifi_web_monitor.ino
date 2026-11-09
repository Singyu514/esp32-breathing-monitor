#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include "model.h"

// Hardware Configuration
#define MPU_ADDR 0x68
#define I2C_SDA  8
#define I2C_SCL  9
#define RGB_BUILTIN 48

// Wi-Fi SoftAP Configuration
const char* AP_SSID = "ESP32_Breathing_Monitor";
const char* AP_PASS = ""; // Open network for easy lab access

WebServer server(80);

// Sampling & Windowing parameters
const unsigned long SAMPLE_INTERVAL_MS = 20; // 50 Hz
#define WINDOW_SIZE 150                      // 3.0 seconds window
#define STEP_SIZE 25                         // Compute inference every 0.5s

// Circular buffer for IMU raw measurements
float ax_buf[WINDOW_SIZE];
float ay_buf[WINDOW_SIZE];
float az_buf[WINDOW_SIZE];
float gx_buf[WINDOW_SIZE];
float gy_buf[WINDOW_SIZE];
float gz_buf[WINDOW_SIZE];

int buf_head = 0;
int sample_count = 0;
unsigned long last_sample_time = 0;

// Current state, smoothed state, and probabilities
int current_decision = STATE_NOT_ATTACHED;
int smoothed_decision = STATE_NOT_ATTACHED;
float current_probs[NUM_CLASSES] = {0.0f, 0.0f, 1.0f, 0.0f};

// Latest sensor sample
float latest_ax = 0.0f, latest_ay = 0.0f, latest_az = 1.0f;
float latest_norm = 1.0f;

// Decision smoothing (majority vote over last 5 predictions = ~2.5s)
#define SMOOTH_HISTORY 5
int history[SMOOTH_HISTORY];
int history_idx = 0;
bool history_full = false;

void set_led_color(int state) {
  #ifdef RGB_BUILTIN
  switch (state) {
    case STATE_CALM_BREATHING:
      neopixelWrite(RGB_BUILTIN, 0, 64, 0);   // Green
      break;
    case STATE_FAST_BREATHING:
      neopixelWrite(RGB_BUILTIN, 64, 0, 0);   // Red
      break;
    case STATE_NOT_ATTACHED:
      neopixelWrite(RGB_BUILTIN, 0, 0, 64);   // Blue
      break;
    case STATE_SHAKING:
      neopixelWrite(RGB_BUILTIN, 64, 0, 64);  // Magenta
      break;
    default:
      neopixelWrite(RGB_BUILTIN, 16, 16, 16);
      break;
  }
  #endif
}

int get_smoothed_decision(int raw_dec) {
  history[history_idx++] = raw_dec;
  if (history_idx >= SMOOTH_HISTORY) {
    history_idx = 0;
    history_full = true;
  }

  int votes[NUM_CLASSES] = {0};
  int total_to_check = history_full ? SMOOTH_HISTORY : history_idx;
  for (int i = 0; i < total_to_check; i++) {
    votes[history[i]]++;
  }

  int best_class = raw_dec;
  int max_votes = 0;
  for (int c = 0; c < NUM_CLASSES; c++) {
    if (votes[c] > max_votes) {
      max_votes = votes[c];
      best_class = c;
    }
  }
  return best_class;
}

void compute_features_and_classify() {
  float acc_norm[WINDOW_SIZE];
  float gyr_norm[WINDOW_SIZE];
  float sum_acc_norm = 0.0f;
  float sum_gyr_norm = 0.0f;
  float min_acc = 1e9f;
  float max_acc = -1e9f;

  for (int i = 0; i < WINDOW_SIZE; i++) {
    int idx = (buf_head + i) % WINDOW_SIZE;
    float an = sqrtf(ax_buf[idx]*ax_buf[idx] + ay_buf[idx]*ay_buf[idx] + az_buf[idx]*az_buf[idx]);
    float gn = sqrtf(gx_buf[idx]*gx_buf[idx] + gy_buf[idx]*gy_buf[idx] + gz_buf[idx]*gz_buf[idx]);

    acc_norm[i] = an;
    gyr_norm[i] = gn;

    sum_acc_norm += an;
    sum_gyr_norm += gn;

    if (an < min_acc) min_acc = an;
    if (an > max_acc) max_acc = an;
  }

  float mean_acc = sum_acc_norm / WINDOW_SIZE;
  float mean_gyr = sum_gyr_norm / WINDOW_SIZE;

  float var_sum = 0.0f;
  float diff_sum = 0.0f;
  int zero_crossings = 0;

  for (int i = 0; i < WINDOW_SIZE; i++) {
    float diff_mean = acc_norm[i] - mean_acc;
    var_sum += diff_mean * diff_mean;

    if (i > 0) {
      diff_sum += fabsf(acc_norm[i] - acc_norm[i-1]);
      float prev_diff = acc_norm[i-1] - mean_acc;
      if ((diff_mean > 0 && prev_diff <= 0) || (diff_mean < 0 && prev_diff >= 0)) {
        zero_crossings++;
      }
    }
  }

  float acc_var = var_sum / WINDOW_SIZE;
  float log_acc_var = logf(acc_var + 1e-7f);
  float acc_p2p = max_acc - min_acc;
  float log_gyr_mean = logf(mean_gyr + 1e-4f);
  float mean_abs_diff = diff_sum / (WINDOW_SIZE - 1);

  // Features vector matching model.h
  float features[NUM_FEATURES];
  features[0] = log_acc_var;
  features[1] = acc_p2p;
  features[2] = mean_gyr;
  features[3] = log_gyr_mean;
  features[4] = (float)zero_crossings;
  features[5] = mean_abs_diff;

  // Run model inference
  current_decision = predict_state(features, current_probs);
  smoothed_decision = get_smoothed_decision(current_decision);

  set_led_color(smoothed_decision);
}

// Embedded Web Page (Self-contained, NO external CDN dependencies)
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-S3 Live Breathing Web Server</title>
  <style>
    :root {
      --bg: #0b0f19; --card: #151c2c; --border: #232f48;
      --calm: #10b981; --fast: #ef4444; --detached: #3b82f6; --shaking: #d946ef;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: #f1f5f9; padding: 1.2rem; display: flex; flex-direction: column; gap: 1.2rem; }
    header { border-bottom: 1px solid var(--border); padding-bottom: 0.8rem; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; }
    h1 { font-size: 1.3rem; }
    .badge { background: #1e293b; color: #38bdf8; border: 1px solid #38bdf8; padding: 0.2rem 0.5rem; border-radius: 99px; font-size: 0.8rem; }
    .card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 1.2rem; display: flex; flex-direction: column; gap: 0.8rem; }
    .card-title { font-size: 0.95rem; text-transform: uppercase; letter-spacing: 0.05em; color: #94a3b8; }
    
    /* Live State Badge */
    .state-badge { padding: 1.2rem; border-radius: 10px; text-align: center; font-size: 1.8rem; font-weight: 800; letter-spacing: 0.05em; border: 2px solid var(--border); transition: all 0.3s; }
    .calm { border-color: var(--calm); color: var(--calm); box-shadow: 0 0 20px rgba(16,185,129,0.2); }
    .fast { border-color: var(--fast); color: var(--fast); box-shadow: 0 0 20px rgba(239,68,68,0.25); }
    .detached { border-color: var(--detached); color: var(--detached); box-shadow: 0 0 20px rgba(59,130,246,0.2); }
    .shaking { border-color: var(--shaking); color: var(--shaking); box-shadow: 0 0 20px rgba(217,70,239,0.25); }

    /* Canvas Signal Waveform */
    canvas { width: 100%; height: 160px; background: #0a0e17; border-radius: 8px; border: 1px solid var(--border); display: block; }

    /* Probability Bars */
    .prob-row { display: flex; flex-direction: column; gap: 0.3rem; font-size: 0.85rem; }
    .prob-header { display: flex; justify-content: space-between; font-weight: 600; }
    .bar-bg { height: 12px; background: #1e293b; border-radius: 6px; overflow: hidden; }
    .bar-fill { height: 100%; width: 0%; transition: width 0.2s ease-out; border-radius: 6px; }
  </style>
</head>
<body>
  <header>
    <h1>🫁 ESP32-S3 Live Web Station <span class="badge">Wi-Fi SoftAP</span></h1>
    <span style="font-size: 0.85rem; color: #94a3b8;">Standalone On-Chip Web Server</span>
  </header>

  <!-- Live State Card -->
  <div class="card">
    <div class="card-title">Real-Time Classification</div>
    <div id="stateBadge" class="state-badge detached">INITIALIZING...</div>
  </div>

  <!-- Live Signal Waveform Card -->
  <div class="card">
    <div style="display: flex; justify-content: space-between; align-items: center;">
      <div class="card-title">Live Sensor Signal (Acceleration Norm)</div>
      <span id="currentVal" style="font-size: 0.85rem; color: #38bdf8;">1.00 g</span>
    </div>
    <canvas id="scopeCanvas" width="600" height="160"></canvas>
  </div>

  <!-- Class Probabilities Card -->
  <div class="card">
    <div class="card-title">Class Probabilities (Confidence Distribution)</div>
    
    <div class="prob-row">
      <div class="prob-header"><span>🟢 Calm Breathing</span><span id="pCalm">0%</span></div>
      <div class="bar-bg"><div id="barCalm" class="bar-fill" style="background: var(--calm);"></div></div>
    </div>

    <div class="prob-row">
      <div class="prob-header"><span>🔴 Fast Breathing (Tachypnea)</span><span id="pFast">0%</span></div>
      <div class="bar-bg"><div id="barFast" class="bar-fill" style="background: var(--fast);"></div></div>
    </div>

    <div class="prob-row">
      <div class="prob-header"><span>🔵 Not Attached (Hand / Table)</span><span id="pDetached">0%</span></div>
      <div class="bar-bg"><div id="barDetached" class="bar-fill" style="background: var(--detached);"></div></div>
    </div>

    <div class="prob-row">
      <div class="prob-header"><span>🟣 Shaking</span><span id="pShaking">0%</span></div>
      <div class="bar-bg"><div id="barShaking" class="bar-fill" style="background: var(--shaking);"></div></div>
    </div>
  </div>

  <script>
    const canvas = document.getElementById('scopeCanvas');
    const ctx = canvas.getContext('2d');
    const history = new Array(120).fill(1.0);

    function drawWaveform(val) {
      history.push(val);
      history.shift();

      ctx.clearRect(0, 0, canvas.width, canvas.height);

      // Grid line at 1.0g
      const midY = canvas.height / 2;
      ctx.strokeStyle = '#1e293b';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(0, midY);
      ctx.lineTo(canvas.width, midY);
      ctx.stroke();

      // Waveform
      ctx.strokeStyle = '#38bdf8';
      ctx.lineWidth = 2.5;
      ctx.beginPath();
      const step = canvas.width / (history.length - 1);

      for (let i = 0; i < history.length; i++) {
        // Map 0.8g -> bottom, 1.2g -> top (scale factor = 200)
        const y = midY - (history[i] - 1.0) * 220;
        const x = i * step;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    async function fetchData() {
      try {
        const res = await fetch('/data');
        if (!res.ok) return;
        const data = await res.json();

        // Update state
        const badge = document.getElementById('stateBadge');
        badge.className = 'state-badge';
        if (data.state === 'calm_breathing') {
          badge.classList.add('calm');
          badge.innerText = '🟢 CALM BREATHING';
        } else if (data.state === 'fast_breathing') {
          badge.classList.add('fast');
          badge.innerText = '🔴 FAST BREATHING';
        } else if (data.state === 'not_attached') {
          badge.classList.add('detached');
          badge.innerText = '🔵 NOT ATTACHED';
        } else if (data.state === 'shaking') {
          badge.classList.add('shaking');
          badge.innerText = '🟣 SHAKING';
        }

        // Update Signal
        document.getElementById('currentVal').innerText = data.norm.toFixed(3) + ' g';
        drawWaveform(data.norm);

        // Update Probabilities
        const p0 = Math.round(data.probs[0] * 100);
        const p1 = Math.round(data.probs[1] * 100);
        const p2 = Math.round(data.probs[2] * 100);
        const p3 = Math.round(data.probs[3] * 100);

        document.getElementById('pCalm').innerText = p0 + '%';
        document.getElementById('barCalm').style.width = p0 + '%';

        document.getElementById('pFast').innerText = p1 + '%';
        document.getElementById('barFast').style.width = p1 + '%';

        document.getElementById('pDetached').innerText = p2 + '%';
        document.getElementById('barDetached').style.width = p2 + '%';

        document.getElementById('pShaking').innerText = p3 + '%';
        document.getElementById('barShaking').style.width = p3 + '%';

      } catch (e) {
        // Fetch error (retrying next cycle)
      }
    }

    setInterval(fetchData, 100);
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  // Return JSON with prediction, probabilities, and live signal
  char json[256];
  snprintf(json, sizeof(json),
           "{\"state\":\"%s\",\"norm\":%.3f,\"probs\":[%.2f,%.2f,%.2f,%.2f]}",
           CLASS_NAMES[smoothed_decision],
           latest_norm,
           current_probs[0],
           current_probs[1],
           current_probs[2],
           current_probs[3]);

  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  // Wake up MPU-6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);

  // Configure Accel (+/- 4g) & Gyro (+/- 500 deg/s)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C);
  Wire.write(0x08);
  Wire.endTransmission(true);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1B);
  Wire.write(0x08);
  Wire.endTransmission(true);

  #ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 64);
  #endif

  // Configure Wi-Fi SoftAP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress myIP = WiFi.softAPIP();
  Serial.println("==================================================");
  Serial.println("  ESP32-S3 SoftAP Web Server Started");
  Serial.print("  Wi-Fi SSID: "); Serial.println(AP_SSID);
  Serial.print("  Server URL: http://"); Serial.println(myIP);
  Serial.println("==================================================");

  // Web Server Endpoints
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - last_sample_time >= SAMPLE_INTERVAL_MS) {
    last_sample_time = now;

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 14, true);

    if (Wire.available() >= 14) {
      int16_t raw_ax = (Wire.read() << 8) | Wire.read();
      int16_t raw_ay = (Wire.read() << 8) | Wire.read();
      int16_t raw_az = (Wire.read() << 8) | Wire.read();
      Wire.read(); Wire.read(); // Skip temp
      int16_t raw_gx = (Wire.read() << 8) | Wire.read();
      int16_t raw_gy = (Wire.read() << 8) | Wire.read();
      int16_t raw_gz = (Wire.read() << 8) | Wire.read();

      latest_ax = raw_ax / 8192.0f;
      latest_ay = raw_ay / 8192.0f;
      latest_az = raw_az / 8192.0f;
      latest_norm = sqrtf(latest_ax * latest_ax + latest_ay * latest_ay + latest_az * latest_az);

      ax_buf[buf_head] = latest_ax;
      ay_buf[buf_head] = latest_ay;
      az_buf[buf_head] = latest_az;
      gx_buf[buf_head] = raw_gx / 65.5f;
      gy_buf[buf_head] = raw_gy / 65.5f;
      gz_buf[buf_head] = raw_gz / 65.5f;

      buf_head = (buf_head + 1) % WINDOW_SIZE;
      sample_count++;

      if (sample_count >= WINDOW_SIZE && (sample_count % STEP_SIZE == 0)) {
        compute_features_and_classify();
      }
    }
  }
}
