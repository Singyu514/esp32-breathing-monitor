import os
import sys
import time
import serial
import serial.tools.list_ports

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "data")
CLASSES = {
    "1": ("calm_breathing", "Sensor against chest, breathing calmly and steadily"),
    "2": ("fast_breathing", "Sensor against chest, breathing fast / hyperventilating"),
    "3": ("not_attached", "Sensor held loosely in hand, or placed still on table"),
    "4": ("shaking", "Sensor being shaken vigorously")
}

def find_esp32_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = p.description.lower()
        if "ch34" in desc or "cp210" in desc or "usb" in desc or "uart" in desc:
            return p.device
    if ports:
        return ports[0].device
    return None

def record_session(port, baudrate=115200, duration_sec=25):
    os.makedirs(DATA_DIR, exist_ok=True)

    print("\n" + "="*50)
    print("  ESP32-S3 Data Collection for Breathing & Motion")
    print("="*50)
    print("Select the state to record:")
    for k, (name, desc) in CLASSES.items():
        print(f"  [{k}] {name.upper()}: {desc}")
    print("  [q] Quit")

    choice = input("\nEnter choice (1-4 or q): ").strip()
    if choice.lower() == 'q':
        return False
    if choice not in CLASSES:
        print("Invalid choice, please select 1, 2, 3, 4, or q.")
        return True

    label = CLASSES[choice][0]

    session_num = input(f"Enter session identifier (e.g. 1, 2, 3) [default: 1]: ").strip()
    if not session_num:
        session_num = "1"

    timestamp_str = time.strftime("%Y%m%d_%H%M%S")
    filename = f"{label}_s{session_num}_{timestamp_str}.csv"
    filepath = os.path.join(DATA_DIR, filename)

    print(f"\nOpening serial port {port} at {baudrate} baud...")
    try:
        ser = serial.Serial(port, baudrate, timeout=1.0)
    except Exception as e:
        print(f"ERROR: Could not open {port}: {e}")
        return False

    time.sleep(1.0)
    ser.reset_input_buffer()

    print(f"\nTarget: {label.upper()}")
    print("Prepare the sensor! Starting in:")
    for i in range(3, 0, -1):
        print(f"  {i}...")
        time.sleep(1.0)
    print(">>> RECORDING NOW! Keep the motion/breathing consistent! <<<")

    samples = []
    start_time = time.time()
    last_print = 0

    while time.time() - start_time < duration_sec:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if not line or "timestamp" in line:
            continue
        parts = line.split(",")
        if len(parts) == 7:
            try:
                # Validate numbers
                [float(x) for x in parts]
                samples.append(line)
            except ValueError:
                pass

        elapsed = time.time() - start_time
        if elapsed - last_print >= 1.0:
            last_print = elapsed
            remaining = duration_sec - elapsed
            print(f"  Recording... {len(samples)} samples collected ({remaining:.0f}s remaining)")

    ser.close()

    if len(samples) < 50:
        print(f"\nWARNING: Only collected {len(samples)} samples. Check sensor/serial connection!")
        return True

    with open(filepath, "w", encoding="utf-8") as f:
        f.write("timestamp_ms,ax,ay,az,gx,gy,gz\n")
        for s in samples:
            f.write(s + "\n")

    print(f"\n Saved {len(samples)} samples to: {filename}")
    print("="*50 + "\n")
    return True

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else find_esp32_port()
    if not port:
        print("ERROR: No serial port found. Make sure ESP32-S3 is plugged in.")
        sys.exit(1)

    print(f"Using serial port: {port}")
    while record_session(port):
        cont = input("Record another session? (y/n) [default: y]: ").strip().lower()
        if cont == 'n':
            break

    print("\nData collection finished.")
