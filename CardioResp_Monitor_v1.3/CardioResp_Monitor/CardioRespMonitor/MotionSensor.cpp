// =====================================================================
//  MotionSensor.cpp - register-level driver of the 6-axis motion sensor (class MPU6050Lite)
// =====================================================================
#include "MotionSensor.h"

enum : uint8_t {
  R_SELF_TEST_X = 0x0D, R_SELF_TEST_Y = 0x0E, R_SELF_TEST_Z = 0x0F, R_SELF_TEST_A = 0x10,
  R_SMPLRT_DIV = 0x19, R_CONFIG = 0x1A, R_GYRO_CONFIG = 0x1B, R_ACCEL_CONFIG = 0x1C, R_ACCEL_CONFIG2 = 0x1D,
  R_FIFO_EN = 0x23, R_INT_PIN_CFG = 0x37, R_INT_ENABLE = 0x38, R_INT_STATUS = 0x3A,
  R_ACCEL_XOUT_H = 0x3B, R_TEMP_OUT_H = 0x41, R_GYRO_XOUT_H = 0x43,
  R_SIGNAL_PATH_RESET = 0x68, R_USER_CTRL = 0x6A, R_PWR_MGMT_1 = 0x6B, R_PWR_MGMT_2 = 0x6C,
  R_FIFO_COUNTH = 0x72, R_FIFO_R_W = 0x74, R_WHO_AM_I = 0x75
};

static bool is6500Family(uint8_t who) {
  return who == 0x70 || who == 0x71 || who == 0x73 || who == 0x74 || who == 0x75 || who == 0x12 || who == 0x19 || who == 0x98;
}

bool MPU6050Lite::write8(uint8_t reg, uint8_t val) {
  m_wire->beginTransmission(m_addr);
  m_wire->write(reg);
  m_wire->write(val);
  if (m_wire->endTransmission(true) != 0) { m_i2cErr++; return false; }
  return true;
}

bool MPU6050Lite::read(uint8_t reg, uint8_t* buf, size_t n) {
  m_wire->beginTransmission(m_addr);
  m_wire->write(reg);
  if (m_wire->endTransmission(false) != 0) { m_i2cErr++; return false; }
  size_t got = m_wire->requestFrom((uint16_t)m_addr, n, true);
  if (got != n) {
    m_i2cErr++;
    while (m_wire->available()) m_wire->read();
    return false;
  }
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)m_wire->read();
  return true;
}

const char* MPU6050Lite::model() const {
  switch (m_who) {
    case 0x68: return "MPU-6050";
    case 0x72: return "MPU-6050 compatible (0x72)";
    case 0x70: return "MPU-6500";
    case 0x71: return "MPU-9250";
    case 0x73: return "MPU-9255";
    case 0x74: return "MPU-6515";
    case 0x98: return "MPU-6050 clone (0x98)";
    case 0x12: return "ICM-20602";
    case 0x19: return "MPU-6886";
    default: return "Unknown MPU compatible";
  }
}

bool MPU6050Lite::begin(TwoWire& wire, uint8_t addr) {
  m_wire = &wire;
  m_addr = addr;
  uint8_t who = 0;
  if (!read(R_WHO_AM_I, &who, 1)) return false;
  if (who == 0x00 || who == 0xFF) return false;
  m_who = who;
  write8(R_PWR_MGMT_1, 0x80);          // device reset
  delay(100);
  write8(R_SIGNAL_PATH_RESET, 0x07);   // reset gyro / accel / temp paths
  delay(100);
  write8(R_PWR_MGMT_1, 0x01);          // wake up, PLL with X-gyro reference (stable clock)
  write8(R_PWR_MGMT_2, 0x00);          // all axes on
  delay(50);
  m_fifoSize = (who == 0x68 || who == 0x72) ? 1024 : 512;
  return configure();
}

bool MPU6050Lite::configure() {
  bool ok = true;
  ok &= write8(R_FIFO_EN, 0x00);
  ok &= write8(R_USER_CTRL, 0x00);
  ok &= write8(R_CONFIG, 0x03);         // DLPF_CFG 3: accel 44 Hz / gyro 42 Hz, internal 1 kHz
  ok &= write8(R_SMPLRT_DIV, 4);        // 1 kHz / (1 + 4) = 200 Hz output data rate
  ok &= write8(R_GYRO_CONFIG, 0x00);    // +-250 dps  = 131 LSB/dps (most sensitive)
  ok &= write8(R_ACCEL_CONFIG, 0x00);   // +-2 g      = 16384 LSB/g (most sensitive)
  if (is6500Family(m_who)) ok &= write8(R_ACCEL_CONFIG2, 0x03);  // accel DLPF 44.8 Hz
  ok &= write8(R_INT_PIN_CFG, 0x00);
  ok &= write8(R_INT_ENABLE, 0x10);     // latch FIFO overflow flag in INT_STATUS
  resetFifo();
  return ok;
}

void MPU6050Lite::resetFifo() {
  write8(R_FIFO_EN, 0x00);
  write8(R_USER_CTRL, 0x04);            // FIFO reset
  delayMicroseconds(200);
  write8(R_USER_CTRL, 0x40);            // FIFO enable
  write8(R_FIFO_EN, 0x78);              // gyro X,Y,Z + accel -> 12 bytes/sample: ax ay az gx gy gz
  uint8_t st;
  read(R_INT_STATUS, &st, 1);           // clear flags
}

int MPU6050Lite::fifoCount() {
  uint8_t b[2];
  if (!read(R_FIFO_COUNTH, b, 2)) return -1;
  return ((int)b[0] << 8) | b[1];
}

bool MPU6050Lite::fifoOverflow() {
  uint8_t st = 0;
  if (!read(R_INT_STATUS, &st, 1)) return false;
  return (st & 0x10) != 0;
}

int MPU6050Lite::readFifo(int16_t* out, int maxSamples) {
  int cnt = fifoCount();
  if (cnt < 0) return -1;
  if (cnt % MPU_SAMPLE_BYTES) {              // may be caught mid-write: look again
    delayMicroseconds(300);
    cnt = fifoCount();
    if (cnt < 0) return -1;
    if (cnt % MPU_SAMPLE_BYTES) return -2;   // really misaligned (after overflow) -> caller resets
  }
  if (cnt >= m_fifoSize - MPU_SAMPLE_BYTES) return -2;   // full: samples were probably dropped
  int n = cnt / MPU_SAMPLE_BYTES;
  if (n > maxSamples) n = maxSamples;
  uint8_t buf[10 * MPU_SAMPLE_BYTES];
  int done = 0;
  while (done < n) {
    int chunk = n - done;
    if (chunk > 10) chunk = 10;             // stay below the 128-byte Wire buffer
    if (!read(R_FIFO_R_W, buf, chunk * MPU_SAMPLE_BYTES)) return done > 0 ? done : -1;
    for (int s = 0; s < chunk; s++) {
      const uint8_t* p = buf + s * MPU_SAMPLE_BYTES;
      int16_t* o = out + (done + s) * 6;
      for (int k = 0; k < 6; k++) o[k] = (int16_t)(((uint16_t)p[2 * k] << 8) | p[2 * k + 1]);
    }
    done += chunk;
  }
  return n;
}

bool MPU6050Lite::readTemperature(float& c) {
  uint8_t b[2];
  if (!read(R_TEMP_OUT_H, b, 2)) return false;
  int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
  c = is6500Family(m_who) ? raw / 333.87f + 21.0f : raw / 340.0f + 36.53f;
  return true;
}

bool MPU6050Lite::readAverage(int16_t acc[3], int16_t gyr[3], int samples, float outA[3], float outG[3]) {
  double sa[3] = {0, 0, 0}, sg[3] = {0, 0, 0};
  int good = 0;
  for (int i = 0; i < samples; i++) {
    uint8_t b[14];
    if (read(R_ACCEL_XOUT_H, b, 14)) {
      for (int k = 0; k < 3; k++) {
        acc[k] = (int16_t)(((uint16_t)b[2 * k] << 8) | b[2 * k + 1]);
        gyr[k] = (int16_t)(((uint16_t)b[8 + 2 * k] << 8) | b[9 + 2 * k]);
        sa[k] += acc[k]; sg[k] += gyr[k];
      }
      good++;
    }
    delay(5);
  }
  if (good == 0) return false;
  for (int k = 0; k < 3; k++) { outA[k] = (float)(sa[k] / good); outG[k] = (float)(sg[k] / good); }
  return true;
}

// Factory self-test (MPU-6000/6050 register map rev 4.2, section 4.1-4.3):
// response = output(self-test on) - output(self-test off), compared with
// the factory trim stored in SELF_TEST_x registers. Pass if within +-14 %.
bool MPU6050Lite::selfTest(MpuSelfTest& r) {
  r = MpuSelfTest();
  if (m_who != 0x68) return false;   // only defined for genuine MPU-6050
  write8(R_FIFO_EN, 0x00);
  write8(R_USER_CTRL, 0x00);
  write8(R_ACCEL_CONFIG, 0x10);      // +-8 g as required by the self-test
  write8(R_GYRO_CONFIG, 0x00);       // +-250 dps
  delay(60);
  int16_t a[3], g[3];
  float aOff[3], gOff[3], aOn[3], gOn[3];
  bool ok = readAverage(a, g, 40, aOff, gOff);
  write8(R_ACCEL_CONFIG, 0xF0);      // XA_ST YA_ST ZA_ST + 8 g
  write8(R_GYRO_CONFIG, 0xE0);       // XG_ST YG_ST ZG_ST + 250 dps
  delay(120);
  ok = ok && readAverage(a, g, 40, aOn, gOn);
  uint8_t st[4] = {0, 0, 0, 0};
  ok = ok && read(R_SELF_TEST_X, st, 4);
  configure();                       // restore normal operation
  if (!ok) return false;

  uint8_t aT[3] = {(uint8_t)(((st[0] >> 3) & 0x1C) | ((st[3] >> 4) & 0x03)),
                   (uint8_t)(((st[1] >> 3) & 0x1C) | ((st[3] >> 2) & 0x03)),
                   (uint8_t)(((st[2] >> 3) & 0x1C) | (st[3] & 0x03))};
  uint8_t gT[3] = {(uint8_t)(st[0] & 0x1F), (uint8_t)(st[1] & 0x1F), (uint8_t)(st[2] & 0x1F)};
  r.ran = true;
  r.passed = true;
  for (int i = 0; i < 3; i++) {
    float ftA = aT[i] ? 4096.0f * 0.34f * powf(0.92f / 0.34f, (aT[i] - 1.0f) / 30.0f) : 0.0f;
    float ftG = gT[i] ? 25.0f * 131.0f * powf(1.046f, gT[i] - 1.0f) : 0.0f;
    if (i == 1) ftG = -ftG;
    float strA = aOn[i] - aOff[i], strG = gOn[i] - gOff[i];
    r.accDev[i] = ftA != 0 ? 100.0f * (strA - ftA) / ftA : 0.0f;
    r.gyroDev[i] = ftG != 0 ? 100.0f * (strG - ftG) / ftG : 0.0f;
    if (ftA == 0 || fabsf(r.accDev[i]) > 14.0f) r.passed = false;
    if (ftG == 0 || fabsf(r.gyroDev[i]) > 14.0f) r.passed = false;
  }
  return true;
}
