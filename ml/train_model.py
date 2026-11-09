import os
import glob
import math
import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit
from sklearn.ensemble import RandomForestClassifier
from sklearn.tree import DecisionTreeClassifier, _tree
from sklearn.metrics import classification_report, confusion_matrix

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "data")
OUTPUT_HEADER = os.path.join(os.path.dirname(__file__), "..", "firmware", "breathing_monitor", "model.h")
DOCS_DIR = os.path.join(os.path.dirname(__file__), "..", "docs")

CLASS_NAMES = ["calm_breathing", "fast_breathing", "not_attached", "shaking"]
CLASS_TO_IDX = {name: i for i, name in enumerate(CLASS_NAMES)}

WINDOW_SIZE = 150  # 3.0 seconds at 50 Hz
STEP_SIZE = 50     # 1.0 second step (66% overlap)

FEATURE_NAMES = [
    "log_acc_var",      # Log variance of acceleration norm
    "acc_p2p",          # Peak-to-peak amplitude (max - min)
    "log_gyr_mean",     # Log mean of gyro magnitude
    "zero_crossings",   # Zero crossings of mean-centered acc norm (frequency indicator)
    "mean_abs_diff",    # Mean absolute difference of successive acc norm samples
    "acc_mean"          # Mean of acc norm (~1.0g)
]

def extract_features(window_data):
    """
    Computes 6 lightweight, robust features for a 3-second window of 50 Hz data.
    window_data: shape (N, 6) -> ax, ay, az, gx, gy, gz
    """
    ax = window_data[:, 0]
    ay = window_data[:, 1]
    az = window_data[:, 2]
    gx = window_data[:, 3]
    gy = window_data[:, 4]
    gz = window_data[:, 5]

    # 1. Acceleration norm
    acc_norm = np.sqrt(ax**2 + ay**2 + az**2)
    acc_var = np.var(acc_norm)
    log_acc_var = np.log(acc_var + 1e-7)
    acc_p2p = np.max(acc_norm) - np.min(acc_norm)
    acc_mean = np.mean(acc_norm)

    # 2. Gyroscope norm
    gyr_norm = np.sqrt(gx**2 + gy**2 + gz**2)
    gyr_mean = np.mean(gyr_norm)
    log_gyr_mean = np.log(gyr_mean + 1e-4)

    # 3. Frequency proxy: zero crossings of mean-centered acc_norm
    centered = acc_norm - acc_mean
    # Count sign changes
    zero_crossings = np.sum(np.diff(centered > 0) != 0)

    # 4. Successive differential energy
    mean_abs_diff = np.mean(np.abs(np.diff(acc_norm)))

    return [
        float(log_acc_var),
        float(acc_p2p),
        float(log_gyr_mean),
        float(zero_crossings),
        float(mean_abs_diff),
        float(acc_mean)
    ]

def load_dataset():
    csv_files = glob.glob(os.path.join(DATA_DIR, "*.csv"))
    if not csv_files:
        print(f"No CSV files found in {DATA_DIR}!")
        return None, None, None, None

    features_list = []
    labels_list = []
    groups_list = [] # Session/file identifier for honest group splitting

    for file_idx, filepath in enumerate(csv_files):
        filename = os.path.basename(filepath)
        # Determine class from filename
        matched_class = None
        for cname in CLASS_NAMES:
            if cname in filename:
                matched_class = cname
                break

        if matched_class is None:
            print(f"Skipping {filename}: class not recognized in filename.")
            continue

        class_idx = CLASS_TO_IDX[matched_class]

        try:
            df = pd.read_csv(filepath)
            # Expect columns: ax, ay, az, gx, gy, gz
            needed_cols = ['ax', 'ay', 'az', 'gx', 'gy', 'gz']
            if not all(col in df.columns for col in needed_cols):
                print(f"Skipping {filename}: missing required columns {needed_cols}")
                continue

            data_matrix = df[needed_cols].values
            num_samples = len(data_matrix)

            if num_samples < WINDOW_SIZE:
                print(f"Skipping {filename}: too short ({num_samples} < {WINDOW_SIZE})")
                continue

            for start in range(0, num_samples - WINDOW_SIZE + 1, STEP_SIZE):
                window = data_matrix[start : start + WINDOW_SIZE]
                feats = extract_features(window)
                features_list.append(feats)
                labels_list.append(class_idx)
                groups_list.append(file_idx)

        except Exception as e:
            print(f"Error reading {filename}: {e}")

    X = np.array(features_list)
    y = np.array(labels_list)
    groups = np.array(groups_list)
    return X, y, groups, csv_files

def tree_to_c(tree, feature_names, tree_idx=0):
    """Recursively converts a scikit-learn DecisionTreeClassifier to pure C code."""
    tree_ = tree.tree_
    feature_name = [
        feature_names[i] if i != _tree.TREE_UNDEFINED else "undefined!"
        for i in tree_.feature
    ]

    lines = []
    def recurse(node, depth):
        indent = "  " * depth
        if tree_.feature[node] != _tree.TREE_UNDEFINED:
            name = feature_name[node]
            threshold = tree_.threshold[node]
            lines.append(f"{indent}if (f[{FEATURE_NAMES.index(name)}] <= {threshold:.6f}f) {{")
            recurse(tree_.children_left[node], depth + 1)
            lines.append(f"{indent}}} else {{")
            recurse(tree_.children_right[node], depth + 1)
            lines.append(f"{indent}}}")
        else:
            # Leaf node: get class probabilities
            values = tree_.value[node][0]
            total = sum(values)
            probs = [v / total if total > 0 else 0 for v in values]
            # Accumulate into votes
            for c_idx, prob in enumerate(probs):
                lines.append(f"{indent}votes[{c_idx}] += {prob:.4f}f;")

    recurse(0, 1)
    return "\n".join(lines)

def export_forest_to_c(rf_model, output_path):
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    n_trees = len(rf_model.estimators_)
    n_classes = len(CLASS_NAMES)

    c_code = f"""// Auto-generated by ml/train_model.py
// Pure C model for ESP32-S3 inference
#ifndef MODEL_H
#define MODEL_H

#define NUM_FEATURES 6
#define NUM_CLASSES 4

// Class IDs
#define STATE_CALM_BREATHING 0
#define STATE_FAST_BREATHING 1
#define STATE_NOT_ATTACHED   2
#define STATE_SHAKING        3

static const char* CLASS_NAMES[] = {{
  "calm_breathing",
  "fast_breathing",
  "not_attached",
  "shaking"
}};

// Feature order:
// 0: log_acc_var
// 1: acc_p2p
// 2: log_gyr_mean
// 3: zero_crossings
// 4: mean_abs_diff
// 5: acc_mean

static inline int predict_state(const float* f, float* out_probs) {{
  float votes[NUM_CLASSES] = {{0.0f, 0.0f, 0.0f, 0.0f}};

"""

    for i, est in enumerate(rf_model.estimators_):
        c_code += f"  // --- Tree {i+1} ---\n"
        c_code += tree_to_c(est, FEATURE_NAMES, tree_idx=i)
        c_code += "\n"

    c_code += f"""
  // Normalize probabilities across {n_trees} trees
  float max_p = -1.0f;
  int best_class = 0;
  for (int c = 0; c < NUM_CLASSES; c++) {{
    float p = votes[c] / {n_trees}.0f;
    if (out_probs != 0) {{
      out_probs[c] = p;
    }}
    if (p > max_p) {{
      max_p = p;
      best_class = c;
    }}
  }}
  return best_class;
}}

#endif // MODEL_H
"""
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(c_code)
    print(f"\n[OK] Model exported to pure C header: {output_path}")

def main():
    print("Loading dataset from", DATA_DIR)
    X, y, groups, files = load_dataset()
    if X is None or len(X) == 0:
        print("Please record data using 'python ml/record_data.py' first!")
        return

    print(f"Total dataset: {len(X)} windows across {len(files)} files.")
    for idx, name in enumerate(CLASS_NAMES):
        count = np.sum(y == idx)
        print(f"  Class {idx} ({name}): {count} windows")

    # Honest split: split by recordings (groups)
    unique_groups = len(np.unique(groups))
    if unique_groups >= 2:
        gss = GroupShuffleSplit(n_splits=1, test_size=0.25, random_state=42)
        train_idx, test_idx = next(gss.split(X, y, groups))
        X_train, X_test = X[train_idx], X[test_idx]
        y_train, y_test = y[train_idx], y[test_idx]
    else:
        print("Warning: Only 1 session file available. Evaluating on whole set.")
        X_train, X_test = X, X
        y_train, y_test = y, y

    print(f"\nTraining Random Forest (5 shallow trees, max_depth=4)...")
    clf = RandomForestClassifier(n_estimators=5, max_depth=4, class_weight='balanced', random_state=42)
    clf.fit(X_train, y_train)

    y_pred = clf.predict(X_test)
    print("\n" + "="*50)
    print("MODEL EVALUATION ON TEST SET (Honest session split):")
    print("="*50)
    print(classification_report(y_test, y_pred, target_names=[CLASS_NAMES[i] for i in sorted(np.unique(y))], zero_division=0))
    print("Confusion Matrix:")
    print(confusion_matrix(y_test, y_pred))
    print("="*50)

    # Re-train on all data before final export
    clf.fit(X, y)
    export_forest_to_c(clf, OUTPUT_HEADER)

if __name__ == "__main__":
    main()
