// =====================================================================
//  MPU6050Lite.h  -  minimal register-level MPU6050 / MPU6500 driver
//  * most sensitive ranges: +-2 g (16384 LSB/g), +-250 dps (131 LSB/dps)
//  * DLPF ~44 Hz, 200 Hz output data rate (1 kHz / (1 + 4))
//  * hardware FIFO (accel + gyro, 12 bytes per sample) so no sample is
//    lost while Wi-Fi / web tasks are busy
//  * factory self-test (genuine MPU6050 only)
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Wire.h>

#define MPU_ACC_LSB_PER_G    16384.0f
#define MPU_GYRO_LSB_PER_DPS 131.0f
#define MPU_SAMPLE_BYTES     12

struct MpuSelfTest {
  bool ran = false;
  bool passed = false;
  float accDev[3] = {0, 0, 0};   // deviation from factory trim (%)
  float gyroDev[3] = {0, 0, 0};
};

class MPU6050Lite {
 public:
  bool begin(TwoWire& wire, uint8_t addr);       // reset + configure; false if not found
  bool configure();                              // ranges, DLPF, rate, FIFO
  bool selfTest(MpuSelfTest& r);                 // leaves the sensor re-configured
  void resetFifo();
  int fifoCount();                               // bytes, -1 on I2C error
  bool fifoOverflow();
  int readFifo(int16_t* out, int maxSamples);    // out: ax ay az gx gy gz per sample
  bool readTemperature(float& c);
  uint8_t whoAmI() const { return m_who; }
  uint8_t address() const { return m_addr; }
  const char* model() const;
  int fifoSize() const { return m_fifoSize; }
  uint32_t i2cErrors() const { return m_i2cErr; }

 private:
  bool write8(uint8_t reg, uint8_t val);
  bool read(uint8_t reg, uint8_t* buf, size_t n);
  bool readAverage(int16_t acc[3], int16_t gyr[3], int samples, float outA[3], float outG[3]);
  TwoWire* m_wire = nullptr;
  uint8_t m_addr = 0x68;
  uint8_t m_who = 0;
  int m_fifoSize = 1024;
  uint32_t m_i2cErr = 0;
};
