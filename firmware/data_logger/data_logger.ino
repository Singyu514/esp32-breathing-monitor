#include <Wire.h>

#define MPU_ADDR 0x68
#define I2C_SDA  8
#define I2C_SCL  9
#define RGB_BUILTIN 48

unsigned long last_sample_time = 0;
const unsigned long SAMPLE_INTERVAL_MS = 20; // 50 Hz

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

  // Configure Gyro (+/- 500 deg/s)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1B);
  Wire.write(0x08);
  Wire.endTransmission(true);

  #ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 16, 16, 16); // White dim LED: Ready to stream
  #endif

  // Header line
  Serial.println("timestamp_ms,ax,ay,az,gx,gy,gz");
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

      // Convert to physical units: g (+/- 4g -> 8192 LSB/g), deg/s (65.5 LSB/(deg/s))
      float ax = raw_ax / 8192.0f;
      float ay = raw_ay / 8192.0f;
      float az = raw_az / 8192.0f;
      float gx = raw_gx / 65.5f;
      float gy = raw_gy / 65.5f;
      float gz = raw_gz / 65.5f;

      Serial.print(now);
      Serial.print(",");
      Serial.print(ax, 4);
      Serial.print(",");
      Serial.print(ay, 4);
      Serial.print(",");
      Serial.print(az, 4);
      Serial.print(",");
      Serial.print(gx, 2);
      Serial.print(",");
      Serial.print(gy, 2);
      Serial.print(",");
      Serial.println(gz, 2);
    }
  }
}
