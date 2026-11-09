#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// Hardware Configuration
#define MPU_ADDR 0x68
#define I2C_SDA  8
#define I2C_SCL  9
#define RGB_BUILTIN 48

// Wi-Fi SoftAP Configuration
const char* AP_SSID = "ESP32_Sleep_Monitor";
const char* AP_PASS = ""; // Open network

WebServer server(80);

// Sleep Positions
#define POS_BACK       0  // Supine (On the back) - Clinical apnea risk
#define POS_STOMACH    1  // Prone (On the stomach)
#define POS_LEFT_SIDE  2  // Left lateral decubitus
#define POS_RIGHT_SIDE 3  // Right lateral decubitus
#define POS_UPRIGHT    4  // Sitting / Standing

const char* POS_NAMES[] = {
  "ON THE BACK (Supine)",
  "ON THE STOMACH (Prone)",
  "ON THE LEFT SIDE",
  "ON THE RIGHT SIDE",
  "UPRIGHT / SITTING"
};

const char* POS_SHORT[] = {
  "Back (Supine)",
  "Stomach (Prone)",
  "Left Side",
  "Right Side",
  "Upright"
};

// Gravity low-pass filter
float grav_x = 0.0f, grav_y = 0.0f, grav_z = 1.0f;
const float ALPHA = 0.05f; // Smoothing factor for gravity isolation

// Current state & timing
int current_position = POS_BACK;
int smoothed_position = POS_BACK;
unsigned long position_start_time = 0;
unsigned long last_sample_time = 0;

// Cumulative statistics (seconds in each position)
unsigned long time_in_pos[5] = {0, 0, 0, 0, 0};
unsigned long last_stat_update = 0;
int total_transitions = 0;

// Night Event Log (stores up to 50 transitions)
struct SleepEvent {
  int position;
  unsigned long timestamp_sec;
  unsigned long duration_sec;
};
#define MAX_LOG_EVENTS 50
SleepEvent night_log[MAX_LOG_EVENTS];
int log_count = 0;

// Position smoothing (majority filter over 1.5 seconds)
#define SMOOTH_HISTORY 6
int pos_history[SMOOTH_HISTORY];
int history_idx = 0;
bool history_full = false;

void set_led_color(int pos) {
  #ifdef RGB_BUILTIN
  switch (pos) {
    case POS_BACK:
      neopixelWrite(RGB_BUILTIN, 0, 0, 64);    // Blue (Supine)
      break;
    case POS_STOMACH:
      neopixelWrite(RGB_BUILTIN, 64, 0, 0);    // Red (Prone)
      break;
    case POS_LEFT_SIDE:
      neopixelWrite(RGB_BUILTIN, 0, 64, 0);    // Green (Left side)
      break;
    case POS_RIGHT_SIDE:
      neopixelWrite(RGB_BUILTIN, 64, 40, 0);   // Yellow/Orange (Right side)
      break;
    case POS_UPRIGHT:
      neopixelWrite(RGB_BUILTIN, 32, 32, 32);  // Dim white (Upright)
      break;
  }
  #endif
}

int classify_sleep_position(float gx, float gy, float gz) {
  // gx: lateral axis (left/right tilt)
  // gy: longitudinal axis (head/foot tilt)
  // gz: normal axis (front/back chest expansion)

  // 1. Check if patient is sitting upright or standing (head up)
  if (fabsf(gy) > 0.65f) {
    return POS_UPRIGHT;
  }

  // 2. Identify dominant horizontal tilt
  float abs_x = fabsf(gx);
  float abs_z = fabsf(gz);

  if (abs_z >= abs_x) {
    // Sensor facing up -> Back (Supine)
    if (gz > 0.0f) {
      return POS_BACK;
    } else {
      return POS_STOMACH;
    }
  } else {
    // Sensor tilted on lateral edge
    if (gx > 0.0f) {
      return POS_LEFT_SIDE;
    } else {
      return POS_RIGHT_SIDE;
    }
  }
}

int get_smoothed_position(int raw_pos) {
  pos_history[history_idx++] = raw_pos;
  if (history_idx >= SMOOTH_HISTORY) {
    history_idx = 0;
    history_full = true;
  }

  int votes[5] = {0};
  int total = history_full ? SMOOTH_HISTORY : history_idx;
  for (int i = 0; i < total; i++) votes[pos_history[i]]++;

  int best = raw_pos;
  int max_v = 0;
  for (int p = 0; p < 5; p++) {
    if (votes[p] > max_v) {
      max_v = votes[p];
      best = p;
    }
  }
  return best;
}

void log_position_change(int old_pos, int new_pos, unsigned long dur_sec) {
  if (log_count < MAX_LOG_EVENTS) {
    night_log[log_count].position = old_pos;
    night_log[log_count].timestamp_sec = millis() / 1000;
    night_log[log_count].duration_sec = dur_sec;
    log_count++;
  } else {
    // Shift log
    for (int i = 0; i < MAX_LOG_EVENTS - 1; i++) {
      night_log[i] = night_log[i + 1];
    }
    night_log[MAX_LOG_EVENTS - 1].position = old_pos;
    night_log[MAX_LOG_EVENTS - 1].timestamp_sec = millis() / 1000;
    night_log[MAX_LOG_EVENTS - 1].duration_sec = dur_sec;
  }
  total_transitions++;

  Serial.println("\n--------------------------------------------------");
  Serial.printf("[NIGHT LOG EVENT] Pos changed: %s -> %s\n", POS_SHORT[old_pos], POS_SHORT[new_pos]);
  Serial.printf("  Time in previous position: %lu seconds\n", dur_sec);
  Serial.println("--------------------------------------------------");
}

// Embedded HTML UI
const char SLEEP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Overnight Sleep Position Monitor - ESP32-S3</title>
  <style>
    :root {
      --bg: #0b0f19; --card: #151c2c; --border: #232f48;
      --back: #3b82f6; --stomach: #ef4444; --left: #10b981; --right: #f59e0b; --upright: #94a3b8;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: #f1f5f9; padding: 1.5rem; display: flex; flex-direction: column; gap: 1.5rem; }
    header { border-bottom: 1px solid var(--border); padding-bottom: 1rem; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.8rem; }
    h1 { font-size: 1.4rem; display: flex; align-items: center; gap: 0.5rem; }
    .badge { background: #1e293b; color: #38bdf8; border: 1px solid #38bdf8; padding: 0.2rem 0.6rem; border-radius: 99px; font-size: 0.8rem; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }
    @media (max-width: 800px) { .grid { grid-template-columns: 1fr; } }
    .card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 1.5rem; display: flex; flex-direction: column; gap: 1rem; }
    .card-title { font-size: 1rem; text-transform: uppercase; letter-spacing: 0.05em; color: #94a3b8; }

    /* Live Position Display */
    .pos-box { display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 2rem; border-radius: 12px; background: #0a0e17; border: 2px solid var(--border); text-align: center; min-height: 170px; }
    .pos-name { font-size: 2rem; font-weight: 800; letter-spacing: 0.05em; margin-bottom: 0.5rem; }
    .pos-dur { font-size: 1.1rem; color: #94a3b8; }
    .pos-back { border-color: var(--back); color: var(--back); box-shadow: 0 0 25px rgba(59,130,246,0.25); }
    .pos-stomach { border-color: var(--stomach); color: var(--stomach); box-shadow: 0 0 25px rgba(239,68,68,0.25); }
    .pos-left { border-color: var(--left); color: var(--left); box-shadow: 0 0 25px rgba(16,185,129,0.25); }
    .pos-right { border-color: var(--right); color: var(--right); box-shadow: 0 0 25px rgba(245,158,11,0.25); }
    .pos-upright { border-color: var(--upright); color: var(--upright); }

    /* Apnea alert banner */
    .apnea-warning { display: none; padding: 0.8rem 1.2rem; border-radius: 8px; background: rgba(239,68,68,0.15); border: 1px solid #ef4444; color: #fca5a5; font-size: 0.9rem; align-items: center; gap: 0.6rem; }

    /* Distribution Progress Bars */
    .dist-row { display: flex; flex-direction: column; gap: 0.3rem; font-size: 0.9rem; }
    .dist-header { display: flex; justify-content: space-between; font-weight: 600; }
    .bar-bg { height: 12px; background: #1e293b; border-radius: 6px; overflow: hidden; }
    .bar-fill { height: 100%; width: 0%; transition: width 0.3s; border-radius: 6px; }

    /* Table for Night Log */
    table { width: 100%; border-collapse: collapse; font-size: 0.85rem; margin-top: 0.5rem; }
    th, td { padding: 0.6rem; text-align: left; border-bottom: 1px solid var(--border); }
    th { color: #94a3b8; font-weight: 600; text-transform: uppercase; font-size: 0.75rem; }
  </style>
</head>
<body>
  <header>
    <div>
      <h1>🛌 Overnight Sleep Position Monitor <span class="badge">Own Extension</span></h1>
      <p style="color: #94a3b8; font-size: 0.85rem; margin-top: 0.2rem;">Computer-Aided Medical Diagnostics - Positional Sleep Apnea Diagnostic</p>
    </div>
    <span id="uptime" style="font-size: 0.85rem; color: #38bdf8;">Recording: 0 s</span>
  </header>

  <div id="apneaAlert" class="apnea-warning">
    <span>⚠️ <strong>Positional Apnea Warning:</strong> Patient has remained in Supine position (on the back) for > 15s. Higher risk of airway collapse.</span>
  </div>

  <div class="grid">
    <!-- Card 1: Live Sleep Posture -->
    <div class="card">
      <div class="card-title">Current Sleep Position</div>
      <div id="posBox" class="pos-box pos-back">
        <div id="posName" class="pos-name">ON THE BACK (Supine)</div>
        <div id="posDur" class="pos-dur">Time in position: 0 s</div>
      </div>
      <div style="font-size: 0.85rem; color: #94a3b8; line-height: 1.4;">
        💡 <strong>Clinical relevance:</strong> In Positional Obstructive Sleep Apnea (POSA), breathing events occur predominantly while sleeping on the back.
      </div>
    </div>

    <!-- Card 2: Cumulative Sleep Distribution -->
    <div class="card">
      <div class="card-title">Night Sleep Position Distribution</div>
      
      <div class="dist-row">
        <div class="dist-header"><span>🔵 On the Back (Supine)</span><span id="pctBack">0%</span></div>
        <div class="bar-bg"><div id="barBack" class="bar-fill" style="background: var(--back);"></div></div>
      </div>

      <div class="dist-row">
        <div class="dist-header"><span>🔴 On the Stomach (Prone)</span><span id="pctStomach">0%</span></div>
        <div class="bar-bg"><div id="barStomach" class="bar-fill" style="background: var(--stomach);"></div></div>
      </div>

      <div class="dist-row">
        <div class="dist-header"><span>🟢 Left Side</span><span id="pctLeft">0%</span></div>
        <div class="bar-bg"><div id="barLeft" class="bar-fill" style="background: var(--left);"></div></div>
      </div>

      <div class="dist-row">
        <div class="dist-header"><span>🟡 Right Side</span><span id="pctRight">0%</span></div>
        <div class="bar-bg"><div id="barRight" class="bar-fill" style="background: var(--right);"></div></div>
      </div>

      <div class="dist-row">
        <div class="dist-header"><span>⚪ Upright / Sitting</span><span id="pctUpright">0%</span></div>
        <div class="bar-bg"><div id="barUpright" class="bar-fill" style="background: var(--upright);"></div></div>
      </div>
    </div>
  </div>

  <!-- Card 3: Night Event Log -->
  <div class="card">
    <div style="display: flex; justify-content: space-between; align-items: center;">
      <div class="card-title">Log of the Night (Transition History)</div>
      <span id="transCount" style="font-size: 0.85rem; color: #94a3b8;">Total transitions: 0</span>
    </div>
    <div style="max-height: 200px; overflow-y: auto;">
      <table>
        <thead>
          <tr>
            <th>Event #</th>
            <th>Sleep Position</th>
            <th>Timestamp</th>
            <th>Duration</th>
          </tr>
        </thead>
        <tbody id="logBody">
          <tr><td colspan="4" style="text-align: center; color: #64748b;">No position change logged yet.</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <script>
    async function updateData() {
      try {
        const res = await fetch('/sleep_data');
        if (!res.ok) return;
        const d = await res.json();

        // Update live position
        const posBox = document.getElementById('posBox');
        const posName = document.getElementById('posName');
        const posDur = document.getElementById('posDur');

        posBox.className = 'pos-box';
        const pNames = ['pos-back', 'pos-stomach', 'pos-left', 'pos-right', 'pos-upright'];
        posBox.classList.add(pNames[d.current_pos]);
        posName.innerText = d.pos_name;
        posDur.innerText = `Time in position: ${d.current_duration} s`;

        // Apnea warning for prolonged back sleeping (> 15s in demo)
        const alertBanner = document.getElementById('apneaAlert');
        if (d.current_pos === 0 && d.current_duration >= 15) {
          alertBanner.style.display = 'flex';
        } else {
          alertBanner.style.display = 'none';
        }

        // Update total time
        document.getElementById('uptime').innerText = `Recording: ${d.total_time} s`;
        document.getElementById('transCount').innerText = `Total transitions: ${d.transitions}`;

        // Update distribution bars
        const t = d.total_time > 0 ? d.total_time : 1;
        const pBack = Math.round((d.time_in_pos[0] / t) * 100);
        const pStom = Math.round((d.time_in_pos[1] / t) * 100);
        const pLeft = Math.round((d.time_in_pos[2] / t) * 100);
        const pRigh = Math.round((d.time_in_pos[3] / t) * 100);
        const pUpri = Math.round((d.time_in_pos[4] / t) * 100);

        document.getElementById('pctBack').innerText = `${d.time_in_pos[0]}s (${pBack}%)`;
        document.getElementById('barBack').style.width = pBack + '%';

        document.getElementById('pctStomach').innerText = `${d.time_in_pos[1]}s (${pStom}%)`;
        document.getElementById('barStomach').style.width = pStom + '%';

        document.getElementById('pctLeft').innerText = `${d.time_in_pos[2]}s (${pLeft}%)`;
        document.getElementById('barLeft').style.width = pLeft + '%';

        document.getElementById('pctRight').innerText = `${d.time_in_pos[3]}s (${pRigh}%)`;
        document.getElementById('barRight').style.width = pRigh + '%';

        document.getElementById('pctUpright').innerText = `${d.time_in_pos[4]}s (${pUpri}%)`;
        document.getElementById('barUpright').style.width = pUpri + '%';

        // Update log table
        if (d.log && d.log.length > 0) {
          const tbody = document.getElementById('logBody');
          tbody.innerHTML = '';
          d.log.slice().reverse().forEach((ev, idx) => {
            const row = document.createElement('tr');
            row.innerHTML = `
              <td>#${d.log.length - idx}</td>
              <td style="font-weight: 600;">${ev.position}</td>
              <td>+${ev.time} s</td>
              <td>${ev.duration} s</td>
            `;
            tbody.appendChild(row);
          });
        }

      } catch (e) {}
    }

    setInterval(updateData, 500);
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", SLEEP_HTML);
}

void handleSleepData() {
  unsigned long now_sec = millis() / 1000;
  unsigned long cur_dur = (millis() - position_start_time) / 1000;
  unsigned long total_time = now_sec;

  // Build JSON
  String json = "{";
  json += "\"current_pos\":" + String(smoothed_position) + ",";
  json += "\"pos_name\":\"" + String(POS_NAMES[smoothed_position]) + "\",";
  json += "\"current_duration\":" + String(cur_dur) + ",";
  json += "\"total_time\":" + String(total_time) + ",";
  json += "\"transitions\":" + String(total_transitions) + ",";
  json += "\"time_in_pos\":[" +
          String(time_in_pos[0]) + "," +
          String(time_in_pos[1]) + "," +
          String(time_in_pos[2]) + "," +
          String(time_in_pos[3]) + "," +
          String(time_in_pos[4]) + "],";

  json += "\"log\":[";
  for (int i = 0; i < log_count; i++) {
    json += "{\"position\":\"" + String(POS_SHORT[night_log[i].position]) + "\",";
    json += "\"time\":" + String(night_log[i].timestamp_sec) + ",";
    json += "\"duration\":" + String(night_log[i].duration_sec) + "}";
    if (i < log_count - 1) json += ",";
  }
  json += "]}";

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

  // Configure Accel (+/- 4g)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C);
  Wire.write(0x08);
  Wire.endTransmission(true);

  #ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 64);
  #endif

  position_start_time = millis();
  last_stat_update = millis();

  // Configure Wi-Fi SoftAP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress myIP = WiFi.softAPIP();
  Serial.println("==================================================");
  Serial.println("  ESP32-S3 Sleep Position Monitor & Night Logger");
  Serial.print("  Wi-Fi SSID: "); Serial.println(AP_SSID);
  Serial.print("  Dashboard URL: http://"); Serial.println(myIP);
  Serial.println("==================================================");

  server.on("/", handleRoot);
  server.on("/sleep_data", handleSleepData);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Update cumulative time stats every second
  if (now - last_stat_update >= 1000) {
    time_in_pos[smoothed_position] += (now - last_stat_update) / 1000;
    last_stat_update = now;
  }

  // Sample IMU at 50 Hz
  if (now - last_sample_time >= SAMPLE_INTERVAL_MS) {
    last_sample_time = now;

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 6, true); // Read ax, ay, az

    if (Wire.available() >= 6) {
      int16_t raw_ax = (Wire.read() << 8) | Wire.read();
      int16_t raw_ay = (Wire.read() << 8) | Wire.read();
      int16_t raw_az = (Wire.read() << 8) | Wire.read();

      float ax = raw_ax / 8192.0f;
      float ay = raw_ay / 8192.0f;
      float az = raw_az / 8192.0f;

      // Low-pass filter to isolate steady gravity vector
      grav_x = (1.0f - ALPHA) * grav_x + ALPHA * ax;
      grav_y = (1.0f - ALPHA) * grav_y + ALPHA * ay;
      grav_z = (1.0f - ALPHA) * grav_z + ALPHA * az;

      int raw_pos = classify_sleep_position(grav_x, grav_y, grav_z);
      int smoothed_pos = get_smoothed_position(raw_pos);

      // Handle position transition
      if (smoothed_pos != smoothed_position) {
        unsigned long dur = (now - position_start_time) / 1000;
        if (dur >= 2) { // only log if held for at least 2 seconds
          log_position_change(smoothed_position, smoothed_pos, dur);
          smoothed_position = smoothed_pos;
          position_start_time = now;
          set_led_color(smoothed_position);
        }
      }
    }
  }
}
