#include "I2C_Driver.h"
#include <Arduino.h>

namespace {
constexpr uint8_t I2C_RETRIES = 3;
constexpr uint32_t I2C_RETRY_DELAY_MS = 2;
}

void I2C_Init(void) {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeOut(50);
}

// Return false on success and true on failure. Keep this convention for the
// existing device drivers, but validate every byte and retry transient NACKs.
bool I2C_Read(uint8_t driverAddress, uint8_t registerAddress,
              uint8_t *registerData, uint32_t length) {
  if (!registerData || length == 0 || length > 255) return true;

  uint8_t lastError = 0;
  size_t lastReceived = 0;
  for (uint8_t attempt = 1; attempt <= I2C_RETRIES; ++attempt) {
    Wire.beginTransmission(driverAddress);
    Wire.write(registerAddress);
    lastError = Wire.endTransmission(false); // Repeated start for register read.
    if (lastError == 0) {
      lastReceived = Wire.requestFrom(driverAddress, (uint8_t)length, true);
      if (lastReceived == length) {
        for (uint32_t i = 0; i < length; ++i) registerData[i] = (uint8_t)Wire.read();
        return false;
      }
      while (Wire.available()) Wire.read();
    }
    if (attempt < I2C_RETRIES) delay(I2C_RETRY_DELAY_MS);
  }

  printf("I2C_READ_FAIL addr=0x%02X reg=0x%02X err=%u received=%u/%u\n",
         driverAddress, registerAddress, lastError,
         (unsigned)lastReceived, (unsigned)length);
  return true;
}

bool I2C_Write(uint8_t driverAddress, uint8_t registerAddress,
               const uint8_t *registerData, uint32_t length) {
  if ((!registerData && length != 0) || length > 255) return true;

  uint8_t lastError = 0;
  for (uint8_t attempt = 1; attempt <= I2C_RETRIES; ++attempt) {
    Wire.beginTransmission(driverAddress);
    Wire.write(registerAddress);
    for (uint32_t i = 0; i < length; ++i) Wire.write(registerData[i]);
    lastError = Wire.endTransmission(true);
    if (lastError == 0) return false;
    if (attempt < I2C_RETRIES) delay(I2C_RETRY_DELAY_MS);
  }

  printf("I2C_WRITE_FAIL addr=0x%02X reg=0x%02X err=%u bytes=%u\n",
         driverAddress, registerAddress, lastError, (unsigned)length);
  return true;
}
