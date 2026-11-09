// Initial baseline model.h (Replaced when running ml/train_model.py)
#ifndef MODEL_H
#define MODEL_H

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

// Features:
// 0: log_acc_var
// 1: acc_p2p
// 2: log_gyr_mean
// 3: zero_crossings
// 4: mean_abs_diff
// 5: acc_mean

static inline int predict_state(const float* f, float* out_probs) {
  // f[0] = log_acc_var
  // f[1] = acc_p2p
  // f[2] = log_gyr_mean
  // f[3] = zero_crossings
  // f[4] = mean_abs_diff
  // f[5] = acc_mean

  float probs[NUM_CLASSES] = {0.0f, 0.0f, 0.0f, 0.0f};
  int decision = STATE_NOT_ATTACHED;

  // Baseline threshold separation
  if (f[4] > 0.35f || f[0] > -1.5f || f[2] > 3.0f) {
    // High dynamic energy / vigorous shake
    decision = STATE_SHAKING;
    probs[STATE_SHAKING] = 0.95f;
    probs[STATE_NOT_ATTACHED] = 0.05f;
  } else if (f[0] < -7.0f && f[1] < 0.04f) {
    // Extremely flat, lying still or held gently without breathing motion
    decision = STATE_NOT_ATTACHED;
    probs[STATE_NOT_ATTACHED] = 0.90f;
    probs[STATE_CALM_BREATHING] = 0.10f;
  } else if (f[3] >= 4.0f || f[4] > 0.025f) {
    // Distinct periodic oscillation with fast frequency
    decision = STATE_FAST_BREATHING;
    probs[STATE_FAST_BREATHING] = 0.85f;
    probs[STATE_CALM_BREATHING] = 0.15f;
  } else {
    // Smooth slow periodic oscillation on chest
    decision = STATE_CALM_BREATHING;
    probs[STATE_CALM_BREATHING] = 0.85f;
    probs[STATE_NOT_ATTACHED] = 0.15f;
  }

  if (out_probs != 0) {
    for (int i = 0; i < NUM_CLASSES; i++) {
      out_probs[i] = probs[i];
    }
  }
  return decision;
}

#endif // MODEL_H
