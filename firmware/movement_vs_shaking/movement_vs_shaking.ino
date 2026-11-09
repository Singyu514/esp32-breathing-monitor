#include <Wire.h>

#define MPU_ADDR 0x68
#define I2C_SDA  8
#define I2C_SCL  9

// Broche de la LED RGB intégrée (ESP32-S3)
#define RGB_BUILTIN 48

// Seuil d'énergie de secousse omnidirectionnelle (ajustable)
#define SHAKE_THRESHOLD 1.2f 

// Variables pour mémoriser l'échantillon précédent (différentiation)
float prev_ax = 0, prev_ay = 0, prev_az = 0;
float prev_gx = 0, prev_gy = 0, prev_gz = 0;

// Fenêtre d'énergie lissée (25 échantillons = 0.5 s à 50 Hz)
#define WINDOW_SIZE 25
float energy_buffer[WINDOW_SIZE];
int buf_index = 0;
bool buf_full = false;

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  // Réveil du MPU-6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);

  // Configuration de l'échelle accéléromètre (+/- 4g) et gyro (+/- 500 deg/s)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); // ACCEL_CONFIG
  Wire.write(0x08); // +/- 4g
  Wire.endTransmission(true);

  #ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 64); // Bleu au démarrage
  #endif

  Serial.println("Systeme omnidirectionnel pret : Bleu = Movement, Magenta = Shaking");
}

void loop() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 14, true);

  if (Wire.available() >= 14) {
    int16_t raw_ax = (Wire.read() << 8) | Wire.read();
    int16_t raw_ay = (Wire.read() << 8) | Wire.read();
    int16_t raw_az = (Wire.read() << 8) | Wire.read();
    Wire.read(); Wire.read(); // Skip température
    int16_t raw_gx = (Wire.read() << 8) | Wire.read();
    int16_t raw_gy = (Wire.read() << 8) | Wire.read();
    int16_t raw_gz = (Wire.read() << 8) | Wire.read();

    // Conversion physique (échelle +/- 4g -> 8192 LSB/g, gyro -> 65.5 LSB/(deg/s))
    float ax = raw_ax / 8192.0f;
    float ay = raw_ay / 8192.0f;
    float az = raw_az / 8192.0f;
    float gx = (raw_gx / 65.5f) / 100.0f; // normalisé
    float gy = (raw_gy / 65.5f) / 100.0f;
    float gz = (raw_gz / 65.5f) / 100.0f;

    // Détection des variations instantanées sur les 3 axes d'accélération + rotation
    float d_acc = fabsf(ax - prev_ax) + fabsf(ay - prev_ay) + fabsf(az - prev_az);
    float d_gyr = fabsf(gx - prev_gx) + fabsf(gy - prev_gy) + fabsf(gz - prev_gz);
    float instant_energy = d_acc + 0.5f * d_gyr;

    prev_ax = ax; prev_ay = ay; prev_az = az;
    prev_gx = gx; prev_gy = gy; prev_gz = gz;

    // Ajout dans le buffer glissant
    energy_buffer[buf_index++] = instant_energy;
    if (buf_index >= WINDOW_SIZE) {
      buf_index = 0;
      buf_full = true;
    }

    if (buf_full) {
      // Énergie moyenne sur la fenêtre
      float mean_energy = 0;
      for (int i = 0; i < WINDOW_SIZE; i++) mean_energy += energy_buffer[i];
      mean_energy /= WINDOW_SIZE;

      if (mean_energy > SHAKE_THRESHOLD) {
        // SHAKING -> MAGENTA (Rouge + Bleu)
        neopixelWrite(RGB_BUILTIN, 64, 0, 64);
        Serial.printf("Etat: SHAKING (Magenta) | Energie: %.3f\n", mean_energy);
      } else {
        // MOVEMENT DANS LA MAIN / POSE -> BLEU
        neopixelWrite(RGB_BUILTIN, 0, 0, 64);
        Serial.printf("Etat: MOVEMENT (Bleu)    | Energie: %.3f\n", mean_energy);
      }
    }
  }

  delay(20); // 50 Hz
}