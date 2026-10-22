#include <Arduino.h>
#include <EEPROM.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

uint32_t fakeMicros = 0;
int fakeAnalog = 512, fakeRcLevel = LOW, fakeButtonLevel = HIGH;
int fakeModeLevel = LOW, fakeThrottleLevel = LOW, fakeGroundLevel = HIGH;
FakeSerial Serial;
FakeEEPROM EEPROM;
#include "firmware_under_test.ino"
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::to_string(__LINE__) + ": " #c); } while (false)
bool near(float a, float b, float e = 0.001f) { return std::fabs(a - b) < e; }
void drain() { while (Serial.room > 0 && telemetrySent < telemetryLength) flushTelemetry(); }
void feed(PwmCapture &channel, int width) {
  if (width < 0) return;
  captureEdge(channel, true, fakeMicros - width);
  captureEdge(channel, false, fakeMicros);
}
void tick(int rc = 1500, int mode = 1000, int throttle = 1000, uint32_t dt = 5000) {
  fakeMicros += dt;
  feed(pitchCapture, rc); feed(modeCapture, mode); feed(throttleCapture, throttle);
  loop(); drain();
}
void issue(const std::string &text) {
  Serial.input += text + "\n";
  for (int i = 0; i < 1000 && (Serial.available() || commandReady || telemetrySent < telemetryLength); ++i) {
    loop(); drain();
  }
  CHECK(!Serial.available() && !commandReady);
}
void ready() {
  setup(); saveCalibration(rawToDeg(fakeAnalog));
  config.throttleSchedule = 1; // 同时回归旧版油门代理模式。
  for (int i = 0; i < 100; ++i) tick(1500, 2000);
  CHECK(controller.sensorGoodSamples == 10 && controller.assistBlend > 0.99f);
}
ControlInput coreInput(int adc = 512, int mode = 2000, int throttle = 1000) {
  ControlInput in = {5000, 5000, (uint16_t)adc, 1500, (uint16_t)mode, (uint16_t)throttle,
    RC_VALID | MODE_VALID | THROTTLE_VALID | CAL_VALID, rawToDeg(512)};
  return in;
}
void testStartup() {
  setup();
  CHECK(!calibrationValid && !groundConfigMode);
  CHECK(canardServo.lastUs == config.servoMidUs);
  CHECK(canardServo.minUs == config.servoMinUs && canardServo.maxUs == config.servoMaxUs);
  for (int i = 0; i < 100; ++i) tick(2000, 1000);
  CHECK(near(controller.commandDeg, -config.limitDeg)); // Manual works without an AOA zero.
  CHECK(controller.assistBlend == 0);
}
void testLegacy() {
  const uint32_t magic = 0xA0A0BEEF;
  EEPROM.put(0, magic);
  for (float zero : {90.0f, -90.0f}) {
    EEPROM.put(4, zero); loadCalibration();
    CHECK(!calibrationValid);
  }
  saveCalibration(12.5f); loadCalibration();
  CHECK(calibrationValid && near(sensorZeroDeg, 12.5f)); // Existing v3 remains compatible.
}
void testRecord() {
  saveCalibration(12.5f);
  CHECK(calibrationValid);
  const auto valid = EEPROM.data;
  EEPROM.data[EEPROM_CAL_ADDR + 6] ^= 1; loadCalibration(); CHECK(!calibrationValid);
  for (int variant = 0; variant < 4; ++variant) {
    EEPROM.data = valid;
    CalibrationRecord r; EEPROM.get(EEPROM_CAL_ADDR, r);
    if (variant == 0) r.version = 2;
    if (variant == 1) r.minDeg = 0;
    if (variant == 2) r.zeroDeg = 180;
    if (variant == 3) r.zeroDeg = std::numeric_limits<float>::quiet_NaN();
    r.crc = calibrationCrc(r); EEPROM.put(EEPROM_CAL_ADDR, r); loadCalibration();
    CHECK(!calibrationValid);
  }
}
void testPowerLoss() {
  saveCalibration(12.5f); const auto original = EEPROM.data;
  for (int cut = 0; cut <= 40; ++cut) {
    EEPROM.data = original; EEPROM.changedWrites = 0; EEPROM.failAfter = cut;
    try { saveCalibration(-87.25f); } catch (const std::runtime_error &) {}
    EEPROM.failAfter = -1; loadCalibration();
    CHECK(!calibrationValid || near(sensorZeroDeg, 12.5f) || near(sensorZeroDeg, -87.25f));
  }
}
void testRcCapture() {
  fakeMicros = 1500; rcISR(); CHECK(!readRcInput().valid);
  fakeRcLevel = HIGH; rcISR(); fakeMicros += 500; fakeRcLevel = LOW; rcISR();
  CHECK(!readRcInput().valid);
  fakeRcLevel = HIGH; rcISR(); fakeMicros += 1500; fakeRcLevel = LOW; rcISR();
  CHECK(readRcInput().valid && readRcInput().pulseUs == 1500);
  fakeMicros += RC_TIMEOUT_US - 1; CHECK(readRcInput().valid);
  ++fakeMicros; CHECK(!readRcInput().valid);
  feed(pitchCapture, 2500); CHECK(!readRcInput().valid);
  feed(pitchCapture, 2000); CHECK(readRcInput().valid);
}
void testRollover() {
  fakeMicros = UINT32_MAX - 700;
  captureEdge(pitchCapture, true, fakeMicros);
  fakeMicros += 1500; captureEdge(pitchCapture, false, fakeMicros);
  CHECK(readRcInput().valid);
  fakeMicros += RC_TIMEOUT_US; CHECK(!readRcInput().valid);
  fakeMicros = pitchCapture.lastUs + 1; CHECK(!readRcInput().valid);
}
void testAuxCapture() {
  fakeModeLevel = HIGH; modeISR(); fakeMicros += 2000; fakeModeLevel = LOW; modeISR();
  CHECK(readChannel(modeCapture).valid && readChannel(modeCapture).pulseUs == 2000);
  fakeThrottleLevel = HIGH; throttleISR(); fakeMicros += 1000; fakeThrottleLevel = LOW; throttleISR();
  CHECK(readChannel(throttleCapture).valid);
  CHECK(!readRcInput().valid);
  fakeMicros += RC_TIMEOUT_US; CHECK(!readChannel(modeCapture).valid && !readChannel(throttleCapture).valid);
}
void testLossOutput() {
  ready(); for (int i = 0; i < 100; ++i) tick(2000, 2000);
  CHECK(near(controller.commandDeg, -config.limitDeg));
  fakeMicros += RC_TIMEOUT_US; loop();
  CHECK(canardServo.lastUs == config.servoMidUs && controller.commandDeg == 0);
}
void testMapping() {
  auto c = defaultConfig();
  CHECK(degToServoUs(c, 0) == c.servoMidUs);
  CHECK(degToServoUs(c, -1000) == degToServoUs(c, -c.limitDeg));
  CHECK(degToServoUs(c, 1000) == degToServoUs(c, c.limitDeg));
  if (c.limitDeg < 90) {
    CHECK(degToServoUs(c, -c.limitDeg) > c.servoMinUs);
    CHECK(degToServoUs(c, c.limitDeg) < c.servoMaxUs);
  } else CHECK(degToServoUs(c, -90) == c.servoMinUs && degToServoUs(c, 90) == c.servoMaxUs);
  for (int width = 1490; width <= 1510; ++width) CHECK(normalizeRc(width) == 0);
  CHECK(near(normalizeRc(800), -1) && near(normalizeRc(2200), 1));
}
void testManualPriority() {
  auto c = defaultConfig(); c.throttleSchedule = 0;
  for (int sign : {-1, 1}) {
    ControllerMemory m = {}; auto in = coreInput(sign > 0 ? 600 : 400);
    in.rcUs = sign > 0 ? 2000 : 1000;
    ControlOutput out = {};
    for (int i = 0; i < 200; ++i) out = controlStep(c, m, in);
    CHECK(near(out.commandDeg, sign > 0 ? -c.limitDeg : c.limitDeg));
    CHECK(near(out.automaticDeg, 0));
  }
}
void testSensor() {
  auto c = defaultConfig(); ControllerMemory m = {}; auto in = coreInput();
  ControlOutput out = {};
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(out.state == ACTIVE);
  in.aoaAdc = 690; out = controlStep(c, m, in); CHECK(out.state == SENSOR_FAULT);
  in.aoaAdc = 0; out = controlStep(c, m, in); CHECK(out.state == SENSOR_FAULT);
  in.aoaAdc = 512;
  for (int i = 0; i < 12; ++i) out = controlStep(c, m, in);
  CHECK(out.state == ACTIVE);
  resetController(m); in.zeroDeg = 179; in.aoaAdc = 3;
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(out.state == ACTIVE && m.aoaFilt > 0 && m.aoaFilt < 3);
}
void testFallback() {
  ready(); fakeAnalog = 0;
  for (int i = 0; i < 100; ++i) tick(2000, 2000);
  CHECK(!controller.sensorGoodSamples && near(controller.commandDeg, -config.limitDeg));
}
void testRate() {
  ready(); fakeAnalog = 540;
  for (int i = 0; i < 100; ++i) {
    const float previous = controller.commandDeg; tick(1500, 2000);
    CHECK(std::fabs(controller.commandDeg - previous) <= 1.801f);
  }
  CHECK(controller.commandDeg < 0);
  const float previous = controller.commandDeg; tick(1000, 2000, 1000, 1000000);
  CHECK(std::fabs(controller.commandDeg - previous) <= 18.001f && missedControlPeriods > 0);
}
void testMode() {
  auto c = defaultConfig(); ControllerMemory m = {}; auto in = coreInput(540);
  ControlOutput out = {};
  for (int i = 0; i < 19; ++i) out = controlStep(c, m, in);
  CHECK(out.state == MANUAL && m.assistBlend == 0);
  out = controlStep(c, m, in); CHECK(out.state == ACTIVE && m.assistBlend > 0 && m.assistBlend < 0.02f);
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(m.assistBlend > 0.99f && out.automaticDeg < 0);
  in.modeUs = 1000; out = controlStep(c, m, in);
  CHECK(out.state == MANUAL && m.assistBlend > 0.9f);
  for (int i = 0; i < 70; ++i) out = controlStep(c, m, in);
  CHECK(out.state == MANUAL && m.assistBlend == 0 && near(out.commandDeg, 0));
  in.modeUs = 1500;
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(out.state == MANUAL);
}
void testModeLoss() {
  auto c = defaultConfig(); ControllerMemory m = {}; auto in = coreInput(540);
  ControlOutput out = {};
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  in.flags &= ~MODE_VALID; in.rcUs = 1750;
  out = controlStep(c, m, in);
  CHECK(out.state == MODE_LOST && out.automaticDeg == 0 && m.assistBlend == 0 && out.manualDeg < 0);
}
void testThrottle() {
  auto c = defaultConfig(); c.throttleSchedule = 1; ControllerMemory m = {}; auto in = coreInput(540);
  ControlOutput out = {};
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(near(out.effectiveGain, c.gain));
  in.throttleUs = 2000; out = controlStep(c, m, in);
  CHECK(out.effectiveGain < c.gain && out.effectiveGain > c.gain * c.throttleFullScale);
  for (int i = 0; i < 1500; ++i) out = controlStep(c, m, in);
  CHECK(near(out.effectiveGain, c.gain * c.throttleFullScale, 0.002f));
  in.flags &= ~THROTTLE_VALID; out = controlStep(c, m, in);
  CHECK(out.state == THROTTLE_LOST && out.effectiveGain == 0);
  c.throttleSchedule = 0;
  for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
  CHECK(out.state == ACTIVE && near(out.effectiveGain, c.gain));
}
void testDirections() {
  auto c = defaultConfig(); c.throttleSchedule = 0;
  const int normal = degToServoUs(c, 20); c.servoDirection = -1;
  CHECK(normal > c.servoMidUs && degToServoUs(c, 20) < c.servoMidUs);
  for (int direction : {-1, 1}) {
    c = defaultConfig(); c.rcDirection = direction;
    ControllerMemory m = {}; auto in = coreInput(512, 1000); in.rcUs = 2000;
    ControlOutput out = {};
    for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
    CHECK(near(out.commandDeg, -direction * c.limitDeg));
    c.aoaDirection = direction; resetController(m); in = coreInput(540);
    for (int i = 0; i < 100; ++i) out = controlStep(c, m, in);
    CHECK(direction * out.commandDeg < 0);
  }
}
void testGroundLock() {
  setup(); CHECK(!groundConfigMode);
  const auto original = config;
  CHECK(!setParameter("servo_dir", -1) && config.servoDirection == original.servoDirection);
  CHECK(!saveConfig());
  beginCalibration({1500, true}, {1000, true}); CHECK(!calibrationActive && calibrationStatus == CAL_REJECTED);
  fakeGroundLevel = LOW; tick();
  CHECK(!groundConfigMode); // Inserting the jumper in RUN cannot reopen configuration.
  issue("set gain 0.9"); CHECK(Serial.output.find("ERR_LOCKED") != std::string::npos);
}
void testGroundRun() {
  fakeGroundLevel = LOW; setup(); tick();
  CHECK(groundConfigMode && canardServo.lastUs == config.servoMidUs);
  CHECK(setParameter("servo_dir", -1));
  issue("run"); CHECK(groundConfigMode);
  fakeGroundLevel = HIGH; tick(1700);
  issue("run"); CHECK(groundConfigMode);
  tick(1500, 2000); issue("run"); CHECK(groundConfigMode);
  tick(); issue("run"); CHECK(!groundConfigMode);
  issue("defaults"); CHECK(!groundConfigMode && config.servoDirection == -1);
}
void testParameters() {
  fakeGroundLevel = LOW; setup();
  CHECK(setParameter("gain", 0.9f) && near(config.gain, 0.9f));
  CHECK(setParameter("limit", 30) && near(config.limitDeg, 30));
  CHECK(setParameter("rc_dir", -1) && setParameter("aoa_dir", -1));
  CHECK(setParameter("throttle_schedule", 0));
  CHECK(!setParameter("servo_dir", 0) && !setParameter("servo_dir", 1.5f));
  CHECK(!setParameter("limit", 100) && !setParameter("gain", NAN));
  CHECK(!setParameter("throttle_schedule", 0.5f));
  CHECK(!setParameter("throttle_full", 1.1f) && !setParameter("servo_min", 502));
  issue("set gain nan"); CHECK(near(config.gain, 0.9f));
  issue("set gain 0.7junk"); CHECK(near(config.gain, 0.9f));
  issue("set gain 0.8 extra"); CHECK(near(config.gain, 0.9f));
  issue(std::string(100, 'x')); CHECK(Serial.output.find("ERR_LONG") != std::string::npos);
  issue("set gain 0.8"); CHECK(near(config.gain, 0.8f));
}
void testConfigPersistence() {
  fakeGroundLevel = LOW; setup();
  CHECK(setParameter("gain", 0.9f)); CHECK(saveConfig());
  CHECK(setParameter("servo_dir", -1)); CHECK(saveConfig());
  config = defaultConfig(); loadConfig();
  CHECK(near(config.gain, 0.9f) && config.servoDirection == -1);
  EEPROM.data[CONFIG_SLOTS[activeConfigSlot] + 12] ^= 1; loadConfig();
  CHECK(near(config.gain, 0.9f) && config.servoDirection == 1);
  EEPROM.data.fill(0xFF);
  LegacyConfigRecord old = {CONFIG_MAGIC, 4, 7, {}, 0};
  auto legacyConfig = defaultConfig(); legacyConfig.gain = 0.7f; legacyConfig.servoDirection = -1;
  memcpy(old.configBytes, &legacyConfig, sizeof(old.configBytes));
  old.crc = crc16(reinterpret_cast<const uint8_t *>(&old), offsetof(LegacyConfigRecord, crc));
  EEPROM.put(CONFIG_SLOTS[0], old); loadConfig();
  CHECK(near(config.gain, 0.7f) && config.servoDirection == -1 && config.speedModel.massGrams == 0);
}
void testConfigPowerLoss() {
  fakeGroundLevel = LOW; setup(); CHECK(setParameter("gain", 0.9f)); CHECK(saveConfig());
  const auto baseline = EEPROM.data;
  const int8_t slot = activeConfigSlot; const uint32_t seq = configSequence;
  for (int cut = 0; cut <= 70; ++cut) {
    EEPROM.data = baseline; activeConfigSlot = slot; configSequence = seq;
    config.gain = 0.7f; EEPROM.changedWrites = 0; EEPROM.failAfter = cut;
    try { saveConfig(); } catch (const std::runtime_error &) {}
    EEPROM.failAfter = -1; loadConfig();
    CHECK(near(config.gain, 0.9f) || near(config.gain, 0.7f));
    CHECK(activeConfigSlot >= 0); // Last committed configuration always survives.
  }
}
void testCalibrationSeam() {
  fakeGroundLevel = LOW; setup();
  controller.aoaFilt = 45;
  beginCalibration({1500, true}, {1000, true}); CHECK(calibrationActive);
  for (int i = 0; i < 32; ++i)
    sampleCalibration(i % 2 ? -179.8f : 179.8f, {1500, true}, {1000, true});
  CHECK(!calibrationActive && calibrationStatus == CAL_SAVED && calibrationValid);
  CHECK(std::fabs(std::fabs(sensorZeroDeg) - 180) < 0.01f && controller.aoaFilt == 0);
}
void testCalibrationReject() {
  fakeGroundLevel = LOW; setup(); saveCalibration(23);
  beginCalibration({1500, false}, {1000, true}); CHECK(!calibrationActive);
  beginCalibration({1500, true}, {2000, true}); CHECK(!calibrationActive);
  beginCalibration({1500, true}, {1000, true});
  sampleCalibration(0, {1500, true}, {1000, true});
  sampleCalibration(3, {1500, true}, {1000, true});
  CHECK(!calibrationActive && near(sensorZeroDeg, 23));
  beginCalibration({1500, true}, {1000, true});
  sampleCalibration(0, {1500, true}, {1000, false}); CHECK(!calibrationActive);
}
void testButton() {
  CHECK(!isCalButtonPressedEvent()); fakeButtonLevel = LOW; CHECK(!isCalButtonPressedEvent());
  fakeMicros = 20000; CHECK(!isCalButtonPressedEvent());
  fakeMicros = 1519000; CHECK(!isCalButtonPressedEvent());
  fakeMicros = 1520000; CHECK(isCalButtonPressedEvent());
  fakeMicros += 2000000; CHECK(!isCalButtonPressedEvent());
}
void testCalibrationIntegration() {
  fakeGroundLevel = LOW; setup(); fakeButtonLevel = LOW;
  for (int i = 0; i < 400; ++i) tick();
  CHECK(calibrationValid && calibrationStatus == CAL_SAVED && !calibrationActive);
  CHECK(near(sensorZeroDeg, rawToDeg(fakeAnalog)) && canardServo.lastUs == config.servoMidUs);
  const int writes = EEPROM.changedWrites;
  for (int i = 0; i < 100; ++i) tick();
  CHECK(EEPROM.changedWrites == writes);
}
void testTelemetry() {
  ready(); Serial.room = 0;
  const uint32_t before = logDropped;
  for (int i = 0; i < 20; ++i) tick(2000, 2000);
  CHECK(controller.commandDeg < 0 && logDropped > before);
  Serial.room = 7; Serial.largestWrite = 0; drain(); tick();
  CHECK(Serial.largestWrite <= 7);
  CHECK(Serial.output.find("C,5,") != std::string::npos && Serial.output.find("R,") != std::string::npos);
}
void testSpeedHistory() {
  auto c = defaultConfig(); c.speedModel = {800, 1000, 20, 5, 2}; c.throttleSchedule = 2;
  CHECK(validConfig(c));
  ControllerMemory m = {}; auto in = coreInput(512, 2000, 2000);
  ControlOutput out = {};
  for (int i = 0; i < 2000; ++i) out = controlStep(c, m, in);
  const float fast = m.speed.speedMps;
  CHECK(fast > 20 && out.state == ACTIVE && m.speed.status == SPEED_PREDICTING);
  in.throttleUs = 1000;
  for (int i = 0; i < 20; ++i) out = controlStep(c, m, in);
  CHECK(m.speed.speedMps > fast * 0.9f); // 收油不能把高速状态立刻变成低速。
  in.modeUs = 1000; controlStep(c, m, in);
  CHECK(m.speed.status == SPEED_PREDICTING); // 手动切换不会清空速度历史。
  in.flags &= ~THROTTLE_VALID; controlStep(c, m, in);
  CHECK(m.speed.status == SPEED_INTERRUPTED);
  in.flags |= THROTTLE_VALID; in.modeUs = 2000;
  for (int i = 0; i < 30; ++i) out = controlStep(c, m, in);
  CHECK(out.state == SPEED_MODEL_UNAVAILABLE);
}
void emitLog(const char *path) {
  setup(); config.speedModel = {800, 1000, 20, 5, 2}; config.throttleSchedule = 2;
  saveCalibration(rawToDeg(fakeAnalog));
  for (int i = 0; i < 100; ++i) tick(1500, 2000);
  fakeAnalog = 540;
  for (int i = 0; i < 100; ++i) tick(1500, 2000, 1000 + i * 10);
  Serial.room = 0;
  for (int i = 0; i < 10; ++i) tick(1700, 2000, 2000);
  Serial.room = 63; drain();
  for (int i = 0; i < 100; ++i) tick(1700, i < 50 ? 1000 : 2000, 1500);
  issue("log stop"); for (int i = 0; i < 10; ++i) tick();
  issue("log start"); for (int i = 0; i < 100; ++i) tick(1500, 2000, 2000);
  fakeAnalog = 0; for (int i = 0; i < 10; ++i) tick();
  std::ofstream file(path); file << Serial.output;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const std::string n = argv[1];
  try {
    if (n == "startup") testStartup();
    else if (n == "legacy") testLegacy();
    else if (n == "record") testRecord();
    else if (n == "power-loss") testPowerLoss();
    else if (n == "rc-capture") testRcCapture();
    else if (n == "rc-rollover") testRollover();
    else if (n == "aux-capture") testAuxCapture();
    else if (n == "rc-loss-output") testLossOutput();
    else if (n == "mapping") testMapping();
    else if (n == "manual-priority") testManualPriority();
    else if (n == "sensor") testSensor();
    else if (n == "manual-fallback") testFallback();
    else if (n == "direction-rate") testRate();
    else if (n == "mode") testMode();
    else if (n == "mode-loss") testModeLoss();
    else if (n == "throttle") testThrottle();
    else if (n == "directions") testDirections();
    else if (n == "ground-lock") testGroundLock();
    else if (n == "ground-run") testGroundRun();
    else if (n == "parameters") testParameters();
    else if (n == "config-persistence") testConfigPersistence();
    else if (n == "config-power-loss") testConfigPowerLoss();
    else if (n == "calibration-seam") testCalibrationSeam();
    else if (n == "calibration-reject") testCalibrationReject();
    else if (n == "calibration-integration") testCalibrationIntegration();
    else if (n == "button") testButton();
    else if (n == "telemetry") testTelemetry();
    else if (n == "speed-history") testSpeedHistory();
    else if (n == "emit-log" && argc == 3) emitLog(argv[2]);
    else return 2;
    std::cout << "PASS " << n << '\n'; return 0;
  } catch (const std::exception &e) { std::cerr << "FAIL " << n << ": " << e.what() << '\n'; return 1; }
}
