#include "Level_IMU.h"

#include <math.h>

#include "I2C_Driver.h"

namespace {
constexpr uint8_t ADDRESSES[] = {0x6B, 0x6A};
constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t REG_CTRL1 = 0x02;
constexpr uint8_t REG_CTRL2 = 0x03;
constexpr uint8_t REG_CTRL3 = 0x04;
constexpr uint8_t REG_CTRL5 = 0x06;
constexpr uint8_t REG_CTRL7 = 0x08;
constexpr uint8_t REG_AX_L = 0x35;
constexpr float RAD_TO_DEG_F = 57.2957795f;

uint8_t deviceAddress = 0;
float fusedAngle = 0.0f;
uint32_t previousUs = 0;
uint32_t lastSampleUs = 0;
bool angleReady = false;
bool sensorEnabled = false;
uint32_t sampleIntervalUs = 8000;
float gyroScaleDps = 256.0f / 32768.0f;
float gravityX = 0.0f, gravityY = 0.0f, gravityZ = 0.0f;
bool gravityReady = false;
uint32_t brakeUntilMs = 0;
constexpr float BRAKE_ACCEL_THRESHOLD_RAW = 1400.0f;
constexpr uint32_t BRAKE_HOLD_MS = 1500;

float wrap180(float angle) {
  while (angle > 180.0f) angle -= 360.0f;
  while (angle < -180.0f) angle += 360.0f;
  return angle;
}

bool writeRegister(uint8_t reg, uint8_t value) {
  return deviceAddress && !I2C_Write(deviceAddress, reg, &value, 1);
}

bool configureProfile(uint8_t accelConfig, uint8_t gyroConfig, uint32_t intervalUs, float gyroScale) {
  const bool ok = writeRegister(REG_CTRL1, 0x40) &&
                  writeRegister(REG_CTRL2, accelConfig) &&
                  writeRegister(REG_CTRL3, gyroConfig) &&
                  writeRegister(REG_CTRL5, 0x77) &&
                  writeRegister(REG_CTRL7, 0x43);
  if (ok) {
    sampleIntervalUs = intervalUs;
    gyroScaleDps = gyroScale;
    previousUs = micros();
    lastSampleUs = previousUs - sampleIntervalUs;
    gravityReady = false;
    brakeUntilMs = 0;
    sensorEnabled = true;
  }
  return ok;
}
}

bool LevelIMU_Init() {
  for (uint8_t address : ADDRESSES) {
    uint8_t who = 0;
    if (!I2C_Read(address, REG_WHO_AM_I, &who, 1) && who != 0x00 && who != 0xFF) {
      deviceAddress = address;
      Serial.printf("QMI8658: address=0x%02X, id=0x%02X\n", address, who);
      break;
    }
  }
  if (!deviceAddress) {
    Serial.println("QMI8658 not found; level lock disabled");
    return false;
  }

  const bool ok = LevelIMU_SetPerformanceProfile();
  delay(20);
  return ok;
}

bool LevelIMU_SetPerformanceProfile() {
  return configureProfile(0x16, 0x46, 8000, 256.0f / 32768.0f);
}

bool LevelIMU_SetBalancedProfile() {
  return configureProfile(0x17, 0x37, 15000, 128.0f / 32768.0f);
}

bool LevelIMU_SetEnabled(bool enabled) {
  if (!deviceAddress) return false;
  if (!enabled) {
    const bool ok = writeRegister(REG_CTRL7, 0x00);
    if (ok) sensorEnabled = false;
    return ok;
  }
  return LevelIMU_SetBalancedProfile();
}

bool LevelIMU_Update(float &angleDegrees) {
  if (!deviceAddress || !sensorEnabled) return false;

  const uint32_t sampleUs = micros();
  if ((uint32_t)(sampleUs - lastSampleUs) < sampleIntervalUs) return false;
  lastSampleUs = sampleUs;

  uint8_t data[12];
  if (I2C_Read(deviceAddress, REG_AX_L, data, sizeof(data))) return false;
  const int16_t axRaw = (int16_t)((uint16_t)data[1] << 8 | data[0]);
  const int16_t ayRaw = (int16_t)((uint16_t)data[3] << 8 | data[2]);
  const int16_t azRaw = (int16_t)((uint16_t)data[5] << 8 | data[4]);
  const int16_t gzRaw = (int16_t)((uint16_t)data[11] << 8 | data[10]);

  // Estimate the gravity vector slowly. The remaining vector is transient
  // acceleration; without a fixed vehicle-forward axis it detects strong
  // braking and other sharp motion alike.
  if (!gravityReady) {
    gravityX = axRaw; gravityY = ayRaw; gravityZ = azRaw;
    gravityReady = true;
  } else {
    constexpr float GRAVITY_FILTER = 0.025f;
    gravityX += ((float)axRaw - gravityX) * GRAVITY_FILTER;
    gravityY += ((float)ayRaw - gravityY) * GRAVITY_FILTER;
    gravityZ += ((float)azRaw - gravityZ) * GRAVITY_FILTER;
    const float dx = axRaw - gravityX, dy = ayRaw - gravityY, dz = azRaw - gravityZ;
    if (dx * dx + dy * dy + dz * dz >= BRAKE_ACCEL_THRESHOLD_RAW * BRAKE_ACCEL_THRESHOLD_RAW)
      brakeUntilMs = millis() + BRAKE_HOLD_MS;
  }

  // When the display is almost horizontal, gravity is perpendicular to its
  // plane and cannot define an on-screen "up" direction. Hold the last angle.
  if ((int64_t)axRaw * axRaw + (int64_t)ayRaw * ayRaw < 1500LL * 1500LL) return false;

  // Gravity projected onto the display plane gives its absolute rotation.
  // QMI8658 Z gyro is perpendicular to the display and smooths quick motion.
  const float accelAngle = atan2f((float)axRaw, (float)ayRaw) * RAD_TO_DEG_F;
  const uint32_t nowUs = micros();
  float dt = (nowUs - previousUs) * 1.0e-6f;
  previousUs = nowUs;
  if (dt <= 0.0f || dt > 0.25f) dt = sampleIntervalUs * 1.0e-6f;

  if (!angleReady) {
    fusedAngle = accelAngle;
    angleReady = true;
  } else {
    const float gyroDps = gzRaw * gyroScaleDps;
    const float predicted = wrap180(fusedAngle + gyroDps * dt);
    const float error = wrap180(accelAngle - predicted);
    fusedAngle = wrap180(predicted + error * 0.08f);
  }
  angleDegrees = fusedAngle;
  return true;
}

bool LevelIMU_IsBrakeDetected() {
  return sensorEnabled && (int32_t)(brakeUntilMs - millis()) > 0;
}
