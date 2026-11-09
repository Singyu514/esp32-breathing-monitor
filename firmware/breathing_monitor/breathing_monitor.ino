#include <Wire.h>
#include <math.h>
#include "model.h"

// Hardware Configuration (same as Part 1)
#define MPU_ADDR 0x68
#define I2C_SDA  8
#define I2C_SCL  9
#define RGB_BUILTIN 48

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
      neopixelWrite(RGB_BUILTIN, 64, 0, 64);  // Magenta (Red + Blue)
      break;
    default:
      neopixelWrite(RGB_BUILTIN, 16, 16, 16); // Dim white
      break;
  }
  #endif
}

int get_smoothed_decision(int current_decision) {
  history[history_idx++] = current_decision;
  if (history_idx >= SMOOTH_HISTORY) {
    history_idx = 0;
    history_full = true;
  }

  // Count votes
  int votes[NUM_CLASSES] = {0};
  int total_to_check = history_full ? SMOOTH_HISTORY : history_idx;
  for (int i = 0; i < total_to_check; i++) {
    votes[history[i]]++;
  }

  // Find class with most votes
  int best_class = current_decision;
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
    // Read from oldest to newest in circular buffer
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

  // Variance of acc_norm, zero crossings, and mean abs diff
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

  // Pack features vector (matches model.h)
  float features[NUM_FEATURES];
  features[0] = log_acc_var;
  features[1] = acc_p2p;
  features[2] = mean_gyr;
  features[3] = log_gyr_mean;
  features[4] = (float)zero_crossings;
  features[5] = mean_abs_diff;

  // Run model inference
  float probs[NUM_CLASSES];
  int raw_decision = predict_state(features, probs);
  int smoothed_decision = get_smoothed_decision(raw_decision);

  // Update RGB LED
  set_led_color(smoothed_decision);

  // Serial feedback
  Serial.printf("State: %-15s [Calm: %.2f | Fast: %.2f | NotAttached: %.2f | Shaking: %.2f]\n",
                CLASS_NAMES[smoothed_decision],
                probs[STATE_CALM_BREATHING],
                probs[STATE_FAST_BREATHING],
                probs[STATE_NOT_ATTACHED],
                probs[STATE_SHAKING]);
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
  neopixelWrite(RGB_BUILTIN, 0, 0, 64); // Start with Blue (Not attached)
  #endif

  Serial.println("==================================================");
  Serial.println("  ESP32-S3 Real-Time Breathing & Motion Monitor");
  Serial.println("  Green: Calm | Red: Fast | Blue: NotAttached | Magenta: Shake");
  Serial.println("==================================================");
}

void loop() {
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

      // Convert to physical units
      ax_buf[buf_head] = raw_ax / 8192.0f;
      ay_buf[buf_head] = raw_ay / 8192.0f;
      az_buf[buf_head] = raw_az / 8192.0f;
      gx_buf[buf_head] = raw_gx / 65.5f;
      gy_buf[buf_head] = raw_gy / 65.5f;
      gz_buf[buf_head] = raw_gz / 65.5f;

      buf_head = (buf_head + 1) % WINDOW_SIZE;
      sample_count++;

      // When window is primed, run inference every STEP_SIZE samples
      if (sample_count >= WINDOW_SIZE && (sample_count % STEP_SIZE == 0)) {
        compute_features_and_classify();
      }
    }
  }
}
