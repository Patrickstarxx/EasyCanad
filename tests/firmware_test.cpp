#include <Arduino.h>
#include <EEPROM.h>
#include <iostream>
#include <limits>
#include <stdexcept>

uint32_t fakeMicros = 0;
int fakeAnalog = 512;
int fakeRcLevel = LOW;
int fakeButtonLevel = HIGH;
FakeSerial Serial;
FakeEEPROM EEPROM;

// The runner stages the actual sketch here, including a reduced-limit installation variant.
#include "firmware_under_test.ino"

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

bool near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) < tolerance; }

void pulse(uint16_t width) {
  fakeRcLevel = HIGH;
  rcISR();
  fakeMicros += width;
  fakeRcLevel = LOW;
  rcISR();
}

void tick(uint16_t rcUs = RC_MID_US, uint32_t elapsed = CONTROL_PERIOD_US) {
  fakeMicros += elapsed - rcUs;
  pulse(rcUs);
  loop();
}

void ready() {
  setup();
  saveCalibration(rawToDeg(fakeAnalog));
  for (int i = 0; i < 12; ++i) tick();
  CHECK(sensorGoodSamples == SENSOR_RECOVERY_SAMPLES);
}

void storeRecord(CalibrationRecord record) {
  record.crc = calibrationCrc(record);
  EEPROM.put(EEPROM_CAL_ADDR, record);
  loadCalibration();
}

void testStartup() {
  setup();
  CHECK(!calibrationValid);
  CHECK(canardServo.lastUs == SERVO_MID_US);
  CHECK(canardServo.minUs == SERVO_MIN_US && canardServo.maxUs == SERVO_MAX_US);
  for (int i = 0; i < 100; ++i) tick(2000);
  CHECK(canardServo.lastUs == SERVO_MID_US);
}

void testLegacy() {
  const uint32_t oldMagic = 0xA0A0BEEF;
  EEPROM.put(0, oldMagic);
  for (float oldZero : {90.0f, -90.0f}) {
    EEPROM.put(4, oldZero);
    loadCalibration();
    CHECK(!calibrationValid && sensorZeroDeg == 0);
  }
  setup();
  tick(2000);
  CHECK(canardServo.lastUs == SERVO_MID_US);
}

void testRecordValidation() {
  saveCalibration(12.5f);
  CHECK(calibrationValid && near(sensorZeroDeg, 12.5f));
  const auto validBytes = EEPROM.data;
  EEPROM.data[EEPROM_CAL_ADDR + offsetof(CalibrationRecord, zeroDeg)] ^= 1;
  loadCalibration();
  CHECK(!calibrationValid);
  EEPROM.data = validBytes;
  CalibrationRecord record;
  EEPROM.get(EEPROM_CAL_ADDR, record);
  const CalibrationRecord validRecord = record;
  record.version = 2;
  storeRecord(record);
  CHECK(!calibrationValid);
  record = validRecord;
  record.minDeg = 0;
  storeRecord(record);
  CHECK(!calibrationValid);
  record = validRecord;
  record.offsetDeg = 1;
  storeRecord(record);
  CHECK(!calibrationValid);
  record = validRecord;
  for (float invalid : {180.0f, -181.0f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
    record.zeroDeg = invalid;
    storeRecord(record);
    CHECK(!calibrationValid);
  }
}

void testPowerLoss() {
  saveCalibration(12.5f);
  const auto original = EEPROM.data;
  for (int cut = 0; cut <= 40; ++cut) {
    EEPROM.data = original;
    EEPROM.changedWrites = 0;
    EEPROM.failAfter = cut;
    try { saveCalibration(-87.25f); } catch (const std::runtime_error &) {}
    EEPROM.failAfter = -1;
    loadCalibration();
    CHECK(!calibrationValid || near(sensorZeroDeg, 12.5f) || near(sensorZeroDeg, -87.25f));
  }
}

void testRcCapture() {
  fakeMicros = 1500;
  rcISR(); // A falling edge without a matching rising edge must be ignored.
  CHECK(!readRcInput().valid);
  pulse(500);
  CHECK(!readRcInput().valid);
  pulse(1500);
  CHECK(readRcInput().valid && readRcInput().pulseUs == 1500);
  fakeMicros += RC_TIMEOUT_US - 1;
  CHECK(readRcInput().valid);
  ++fakeMicros;
  CHECK(!readRcInput().valid);
  pulse(2500);
  CHECK(!readRcInput().valid);
  pulse(2000);
  CHECK(readRcInput().valid && readRcInput().pulseUs == 2000);
}

void testRcRollover() {
  fakeMicros = UINT32_MAX - 700;
  pulse(1500);
  CHECK(readRcInput().valid);
  fakeMicros += RC_TIMEOUT_US;
  CHECK(!readRcInput().valid);
  fakeMicros = rcLastValidMicros + 1; // Even a later full timer wrap cannot revive stale data.
  CHECK(!readRcInput().valid);
  pulse(1000);
  CHECK(readRcInput().valid);
}

void testRcLossOutput() {
  ready();
  for (int i = 0; i < 100; ++i) tick(2000);
  CHECK(near(lastCommandDeg, -canardLimitDeg));
  CHECK(canardServo.lastUs == degToServoUs(-canardLimitDeg));
  fakeMicros += RC_TIMEOUT_US;
  loop();
  CHECK(canardServo.lastUs == SERVO_MID_US && lastCommandDeg == 0);
  CHECK(!readRcInput().valid);
}

void testMappingAndMixing() {
  CHECK(degToServoUs(0) == SERVO_MID_US);
  CHECK(degToServoUs(-1000) == degToServoUs(-canardLimitDeg));
  CHECK(degToServoUs(1000) == degToServoUs(canardLimitDeg));
  if (canardLimitDeg < 90) {
    CHECK(degToServoUs(-canardLimitDeg) > SERVO_MIN_US);
    CHECK(degToServoUs(canardLimitDeg) < SERVO_MAX_US);
  } else {
    CHECK(degToServoUs(-90) == SERVO_MIN_US);
    CHECK(degToServoUs(90) == SERVO_MAX_US);
  }
  for (int pulseUs = 1490; pulseUs <= 1510; ++pulseUs) CHECK(normalizeRc(pulseUs) == 0);
  CHECK(near(normalizeRc(800), -1) && near(normalizeRc(2200), 1));
  CHECK(normalizeRc(1511) > 0 && normalizeRc(1489) < 0);
  for (int rc = -100; rc <= 100; ++rc) {
    for (int aoa = -90; aoa <= 90; ++aoa) {
      const float command = mixedCommand(rc / 100.0f, static_cast<float>(aoa));
      CHECK(std::fabs(command) <= canardLimitDeg + 0.001f);
    }
  }
  CHECK(near(mixedCommand(1, -90), -canardLimitDeg));
  CHECK(near(mixedCommand(-1, 90), canardLimitDeg));
  CHECK(near(mixedCommand(0, 10), -12));
}

void testSensor() {
  sensorZeroDeg = 0;
  resetSensorFilter();
  for (int i = 0; i < SENSOR_RECOVERY_SAMPLES - 1; ++i) CHECK(!updateSensor(0, 5000));
  CHECK(updateSensor(0, 5000));
  CHECK(!updateSensor(60, 5000)); // Sudden but in-range jump.
  CHECK(sensorGoodSamples == 0);
  CHECK(!updateSensor(120, 5000)); // Outside the supported relative AOA range.
  resetSensorFilter();
  sensorZeroDeg = 179;
  for (int i = 0; i < SENSOR_RECOVERY_SAMPLES; ++i) updateSensor(-179, 5000);
  CHECK(sensorGoodSamples == SENSOR_RECOVERY_SAMPLES && near(aoaFilt, 2));
  sensorZeroDeg = 0;
  resetSensorFilter();
  updateSensor(0, 5000);
  updateSensor(10, 5000);
  CHECK(near(aoaFilt, 1.5f));
  resetSensorFilter();
  updateSensor(0, 5000);
  updateSensor(10, 10000);
  CHECK(aoaFilt > 1.5f); // Filter follows elapsed time, not the number of iterations.
}

void testManualFallback() {
  ready();
  fakeAnalog = 0; // -180 degrees relative to the calibrated center: implausible AOA.
  for (int i = 0; i < 100; ++i) tick(2000);
  CHECK(sensorGoodSamples == 0);
  CHECK(near(lastCommandDeg, -canardLimitDeg));
  fakeAnalog = 512;
  for (int i = 0; i < SENSOR_RECOVERY_SAMPLES + 1; ++i) tick();
  CHECK(sensorGoodSamples == SENSOR_RECOVERY_SAMPLES);
}

void testDirectionAndRate() {
  ready();
  fakeAnalog = 540;
  for (int i = 0; i < 100; ++i) {
    const float before = lastCommandDeg;
    tick();
    CHECK(std::fabs(lastCommandDeg - before) <= 1.801f);
  }
  CHECK(lastCommandDeg < 0 && canardServo.lastUs < SERVO_MID_US);
  const float before = lastCommandDeg;
  tick(1000, 1000000);
  CHECK(std::fabs(lastCommandDeg - before) <= 18.001f); // Delays cannot authorize an unbounded jump.
  CHECK(missedControlPeriods > 0 && maxControlIntervalUs >= 1000000);
}

void testCalibrationSeam() {
  setup();
  aoaFilt = 45;
  sensorGoodSamples = SENSOR_RECOVERY_SAMPLES;
  beginCalibration({1500, true});
  CHECK(calibrationActive && canardServo.lastUs == SERVO_MID_US);
  for (int i = 0; i < CAL_SAMPLE_COUNT; ++i) sampleCalibration(i % 2 ? -179.8f : 179.8f, {1500, true});
  CHECK(!calibrationActive && calibrationStatus == CAL_SAVED && calibrationValid);
  CHECK(std::fabs(std::fabs(sensorZeroDeg) - 180) < 0.01f);
  CHECK(aoaFilt == 0 && sensorGoodSamples == 0 && !havePreviousRaw);
}

void testCalibrationReject() {
  saveCalibration(23);
  beginCalibration({1500, false});
  CHECK(!calibrationActive && calibrationStatus == CAL_REJECTED);
  beginCalibration({1700, true});
  CHECK(!calibrationActive);
  beginCalibration({1500, true});
  sampleCalibration(0, {1500, true});
  sampleCalibration(3, {1500, true});
  CHECK(!calibrationActive && calibrationStatus == CAL_REJECTED);
  CHECK(near(sensorZeroDeg, 23));
  beginCalibration({1500, true});
  sampleCalibration(0, {1500, false});
  CHECK(!calibrationActive);
  beginCalibration({1500, true});
  sampleCalibration(0, {1700, true});
  CHECK(!calibrationActive && near(sensorZeroDeg, 23));
}

void testButton() {
  CHECK(!isCalButtonPressedEvent());
  fakeButtonLevel = LOW;
  CHECK(!isCalButtonPressedEvent());
  fakeMicros = 10000;
  fakeButtonLevel = HIGH;
  CHECK(!isCalButtonPressedEvent());
  fakeMicros = 15000;
  fakeButtonLevel = LOW;
  CHECK(!isCalButtonPressedEvent());
  fakeMicros = 35000;
  CHECK(!isCalButtonPressedEvent());
  fakeMicros = 1534000;
  CHECK(!isCalButtonPressedEvent());
  fakeMicros = 1535000;
  CHECK(isCalButtonPressedEvent());
  fakeMicros += 2000000;
  CHECK(!isCalButtonPressedEvent()); // A held button only produces one event.
}

void testTelemetry() {
  ready();
  Serial.room = 0;
  fakeMicros = 200000;
  missedControlPeriods = UINT32_MAX;
  maxControlIntervalUs = UINT32_MAX;
  queueTelemetry(ACTIVE, {1500, true}, 90, SERVO_MAX_US);
  CHECK(telemetryLength > 0 && telemetryLength < sizeof(telemetry));
  flushTelemetry();
  CHECK(telemetrySent == 0);
  for (int i = 0; i < 20; ++i) tick(2000);
  CHECK(lastCommandDeg < 0 && telemetrySent == 0); // Backpressure does not stop control.
  Serial.room = 7;
  while (telemetrySent < telemetryLength) flushTelemetry();
  CHECK(Serial.largestWrite <= 7);
  CHECK(Serial.output.find("State=") != std::string::npos);
  CHECK(Serial.output.back() == '\n');
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string name = argv[1];
  try {
    if (name == "startup") testStartup();
    else if (name == "legacy") testLegacy();
    else if (name == "record") testRecordValidation();
    else if (name == "power-loss") testPowerLoss();
    else if (name == "rc-capture") testRcCapture();
    else if (name == "rc-rollover") testRcRollover();
    else if (name == "rc-loss-output") testRcLossOutput();
    else if (name == "mapping-mixing") testMappingAndMixing();
    else if (name == "sensor") testSensor();
    else if (name == "manual-fallback") testManualFallback();
    else if (name == "direction-rate") testDirectionAndRate();
    else if (name == "calibration-seam") testCalibrationSeam();
    else if (name == "calibration-reject") testCalibrationReject();
    else if (name == "button") testButton();
    else if (name == "telemetry") testTelemetry();
    else return 2;
    std::cout << "PASS " << name << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    return 1;
  }
}
