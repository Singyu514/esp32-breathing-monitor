// Refined physiological classifier for ESP32-S3 inference
// Handles:
// - Shaking (vigorous movement) -> Magenta
// - Hand movement off-chest (high gyro rotation, not on chest) -> Blue
// - Table still (low gyro, very low accel variance) -> Blue
// - Chest Calm breathing (sensor on chest, gentle rhythmic oscillation) -> Green
// - Chest Fast breathing (sensor on chest, strong rapid expansion) -> Red

#ifndef MODEL_H
#define MODEL_H

#include <math.h>

#define NUM_FEATURES 6
#define NUM_CLASSES 4

#define STATE_CALM_BREATHING 0
#define STATE_FAST_BREATHING 1
#define STATE_NOT_ATTACHED   2
#define STATE_SHAKING        3

static const char* CLASS_NAMES[] = {
  "calm_breathing",
  "fast_breathing",
  "not_attached",
  "shaking"
};

// Feature indices:
// f[0] = log_acc_var   (log variance of accel norm)
// f[1] = acc_p2p       (peak-to-peak amplitude of accel norm)
// f[2] = log_gyr_mean  (log mean gyro norm)
// f[3] = zero_crossings (zero crossings of centered accel norm)
// f[4] = mean_abs_diff (high-frequency jerk / differentiation)
// f[5] = acc_mean      (gravity baseline ~ 1.0g)

static inline int predict_state(const float* f, float* out_probs) {
  float probs[NUM_CLASSES] = {0.0f, 0.0f, 0.0f, 0.0f};
  int decision = STATE_NOT_ATTACHED;

  float log_acc_var   = f[0];
  float acc_p2p       = f[1];
  float log_gyr_mean  = f[2];
  float mean_abs_diff = f[4];

  // 1. SHAKING: Violent acceleration and high angular rotation
  if (mean_abs_diff > 0.18f || acc_p2p > 1.0f || log_gyr_mean > 4.25f) {
    decision = STATE_SHAKING;
    probs[STATE_SHAKING]        = 0.92f;
    probs[STATE_NOT_ATTACHED]   = 0.05f;
    probs[STATE_FAST_BREATHING] = 0.02f;
    probs[STATE_CALM_BREATHING] = 0.01f;
  }
  // 2. HAND MOVEMENT OFF CHEST:
  // When held or moved in hand, wrist/finger rotation produces log_gyr_mean > 2.10 (gyro > 8.2 deg/s).
  // On the chest, rotation is physically constrained (< 8.0 deg/s).
  else if (log_gyr_mean > 2.10f) {
    decision = STATE_NOT_ATTACHED; // Hand movement -> BLUE
    probs[STATE_NOT_ATTACHED]   = 0.90f;
    probs[STATE_CALM_BREATHING] = 0.05f;
    probs[STATE_FAST_BREATHING] = 0.04f;
    probs[STATE_SHAKING]        = 0.01f;
  }
  // 3. LYING STILL ON TABLE:
  // Completely flat sensor, near zero rotation and minuscule noise p2p
  else if (log_gyr_mean < -0.20f && acc_p2p < 0.035f) {
    decision = STATE_NOT_ATTACHED; // Table still -> BLUE
    probs[STATE_NOT_ATTACHED]   = 0.94f;
    probs[STATE_CALM_BREATHING] = 0.04f;
    probs[STATE_FAST_BREATHING] = 0.01f;
    probs[STATE_SHAKING]        = 0.01f;
  }
  // 4. ON CHEST:
  // Gyroscope is constrained (0.8 <= gyro <= 8.0 deg/s)
  else {
    // FAST BREATHING: Strong chest expansion and high variance
    if (acc_p2p >= 0.065f || log_acc_var >= -9.20f) {
      decision = STATE_FAST_BREATHING;
      probs[STATE_FAST_BREATHING] = 0.88f;
      probs[STATE_CALM_BREATHING] = 0.08f;
      probs[STATE_NOT_ATTACHED]   = 0.03f;
      probs[STATE_SHAKING]        = 0.01f;
    }
    // CALM BREATHING: Subtle periodic chest motion
    else {
      decision = STATE_CALM_BREATHING;
      probs[STATE_CALM_BREATHING] = 0.88f;
      probs[STATE_FAST_BREATHING] = 0.07f;
      probs[STATE_NOT_ATTACHED]   = 0.04f;
      probs[STATE_SHAKING]        = 0.01f;
    }
  }

  if (out_probs != 0) {
    for (int i = 0; i < NUM_CLASSES; i++) {
      out_probs[i] = probs[i];
    }
  }

  return decision;
}

#endif // MODEL_H
