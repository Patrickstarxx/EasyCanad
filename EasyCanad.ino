#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/atomic.h>
#include "ControlCore.h"
#ifdef ARDUINO_ARCH_AVR
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#define KEY_IS(name) (strcmp_P(key, PSTR(name)) == 0)
#else
#define KEY_IS(name) (strcmp(key, name) == 0)
#endif

const uint8_t PIN_AOA_SENSOR = A0;
const uint8_t PIN_SERVO = 9;
const uint8_t PIN_RC_IN = 2;
const uint8_t PIN_MODE_IN = 3;
const uint8_t PIN_CAL_BTN = 4;
const uint8_t PIN_GROUND_CONFIG = 5;
const uint8_t PIN_THROTTLE_IN = 7;
const uint32_t CONTROL_PERIOD_US = 5000;
const uint32_t RC_TIMEOUT_US = 100000;
const uint32_t CAL_HOLD_MS = 1500;
const uint8_t CAL_SAMPLE_COUNT = 32;
const float CAL_STABILITY_DEG = 2;
const uint32_t SERIAL_BAUD = 500000;

// 兼容上一版明确标记坐标系的 v3 零位；v1/v2 的无版本零位继续拒绝。
const int EEPROM_CAL_ADDR = 16;
const uint32_t CAL_MAGIC = 0x45434133UL;
const uint16_t CAL_VERSION = 3;
struct __attribute__((packed)) CalibrationRecord {
  uint32_t magic;
  uint16_t version;
  float zeroDeg;
  float minDeg;
  float maxDeg;
  float offsetDeg;
  uint16_t crc;
};
struct __attribute__((packed)) ConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint32_t sequence;
  ControlConfig config;
  uint16_t crc;
};
const uint32_t CONFIG_MAGIC = 0x45434634UL;
const uint16_t CONFIG_VERSION = 4;
const int CONFIG_SLOTS[2] = {64, 192};
static_assert(sizeof(ConfigRecord) <= 128, "Configuration slots overlap");

struct PwmCapture {
  volatile uint32_t riseUs;
  volatile uint32_t lastUs;
  volatile uint16_t pulseUs;
  volatile bool highSeen;
  volatile bool valid;
};
struct RcInput { uint16_t pulseUs; bool valid; };
enum CalibrationStatus : uint8_t { CAL_IDLE, CAL_BUSY, CAL_SAVED, CAL_REJECTED };

// Arduino 自动原型必须放在自定义类型声明之后。
void captureEdge(PwmCapture &channel, bool high, uint32_t now);
RcInput readChannel(PwmCapture &channel);
uint16_t calibrationCrc(const CalibrationRecord &record);
bool readConfigSlot(uint8_t slot, ConfigRecord &record);
void beginCalibration(const RcInput &rc, const RcInput &mode);
void sampleCalibration(float rawDeg, const RcInput &rc, const RcInput &mode);
void queueLog(const ControlInput &input, const ControllerMemory &before, const ControlOutput &output);

PwmCapture pitchCapture = {0, 0, 1500, false, false};
PwmCapture modeCapture = {0, 0, 1000, false, false};
PwmCapture throttleCapture = {0, 0, 1000, false, false};
ControlConfig config = defaultConfig();
ControllerMemory controller = {};
Servo canardServo;
float sensorZeroDeg = 0;
bool calibrationValid = false;
bool groundConfigMode = false;
bool calibrationActive = false;
CalibrationStatus calibrationStatus = CAL_IDLE;
uint8_t calSamples = 0;
float calAnchorDeg = 0, calSumDelta = 0, calMinDelta = 0, calMaxDelta = 0;
int8_t activeConfigSlot = -1;
uint32_t configSequence = 0;
uint32_t missedControlPeriods = 0;
uint32_t maxControlIntervalUs = 0;
uint32_t logSequence = 0, logDropped = 0;
bool loggingEnabled = true, metadataPending = true, logResync = true;
char telemetry[256];
size_t telemetryLength = 0, telemetrySent = 0;
char commandBuffer[64];
uint8_t commandLength = 0;
bool commandReady = false, commandOverflow = false;

void captureEdge(PwmCapture &channel, bool high, uint32_t now) {
  if (high) { channel.riseUs = now; channel.highSeen = true; }
  else if (channel.highSeen) {
    channel.highSeen = false;
    const uint32_t width = now - channel.riseUs;
    if (width >= 800 && width <= 2200) {
      channel.pulseUs = (uint16_t)width;
      channel.lastUs = now;
      channel.valid = true;
    }
  }
}
void rcISR() { captureEdge(pitchCapture, digitalRead(PIN_RC_IN) == HIGH, micros()); }
void modeISR() { captureEdge(modeCapture, digitalRead(PIN_MODE_IN) == HIGH, micros()); }
void throttleISR() { captureEdge(throttleCapture, digitalRead(PIN_THROTTLE_IN) == HIGH, micros()); }
#ifdef ARDUINO_ARCH_AVR
ISR(PCINT2_vect) { throttleISR(); }
#endif
RcInput readChannel(PwmCapture &channel) {
  RcInput result;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (channel.valid && (uint32_t)(micros() - channel.lastUs) >= RC_TIMEOUT_US) channel.valid = false;
    result.pulseUs = channel.pulseUs;
    result.valid = channel.valid;
  }
  return result;
}
RcInput readRcInput() { return readChannel(pitchCapture); }

uint16_t crc16(const uint8_t *bytes, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= (uint16_t)bytes[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}
uint16_t calibrationCrc(const CalibrationRecord &record) {
  return crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(CalibrationRecord, crc));
}
void loadCalibration() {
  CalibrationRecord record;
  EEPROM.get(EEPROM_CAL_ADDR, record);
  calibrationValid = record.magic == CAL_MAGIC && record.version == CAL_VERSION &&
    record.crc == calibrationCrc(record) && isfinite(record.zeroDeg) &&
    record.zeroDeg >= -180 && record.zeroDeg < 180 &&
    record.minDeg == -180 && record.maxDeg == 180 && record.offsetDeg == 0;
  sensorZeroDeg = calibrationValid ? record.zeroDeg : 0;
  resetController(controller);
  metadataPending = logResync = true;
}
void writeCommittedRecord(int address, const uint8_t *bytes, size_t length, uint32_t magic) {
  const uint32_t invalid = 0;
  EEPROM.put(address, invalid);
  for (size_t i = sizeof(magic); i < length; ++i) EEPROM.update(address + i, bytes[i]);
  EEPROM.put(address, magic);
}
void saveCalibration(float zeroDeg) {
  CalibrationRecord record = {CAL_MAGIC, CAL_VERSION, wrap180(zeroDeg), -180, 180, 0, 0};
  record.crc = calibrationCrc(record);
  writeCommittedRecord(EEPROM_CAL_ADDR, reinterpret_cast<const uint8_t *>(&record), sizeof(record), record.magic);
  loadCalibration();
}
bool readConfigSlot(uint8_t slot, ConfigRecord &record) {
  EEPROM.get(CONFIG_SLOTS[slot], record);
  return record.magic == CONFIG_MAGIC && record.version == CONFIG_VERSION &&
    record.crc == crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(ConfigRecord, crc)) &&
    validConfig(record.config);
}
void loadConfig() {
  ConfigRecord first, second;
  const bool a = readConfigSlot(0, first), b = readConfigSlot(1, second);
  if (!a && !b) { config = defaultConfig(); activeConfigSlot = -1; configSequence = 0; return; }
  const bool useSecond = b && (!a || (int32_t)(second.sequence - first.sequence) > 0);
  activeConfigSlot = useSecond ? 1 : 0;
  const ConfigRecord &record = useSecond ? second : first;
  config = record.config;
  configSequence = record.sequence;
}
bool saveConfig() {
  if (!groundConfigMode || calibrationActive || !validConfig(config)) return false;
  const uint8_t slot = activeConfigSlot == 0 ? 1 : 0;
  ConfigRecord record = {CONFIG_MAGIC, CONFIG_VERSION, configSequence + 1, config, 0};
  record.crc = crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(ConfigRecord, crc));
  writeCommittedRecord(CONFIG_SLOTS[slot], reinterpret_cast<const uint8_t *>(&record), sizeof(record), record.magic);
  ConfigRecord verified;
  if (!readConfigSlot(slot, verified) || verified.sequence != record.sequence) return false;
  activeConfigSlot = slot;
  configSequence = record.sequence;
  return true;
}

bool isCalButtonPressedEvent() {
  static uint8_t lastRead = HIGH, stableState = HIGH;
  static uint32_t changedAt = 0, pressedAt = 0;
  static bool handled = false;
  const uint32_t now = millis();
  const uint8_t reading = digitalRead(PIN_CAL_BTN);
  if (reading != lastRead) { lastRead = reading; changedAt = now; }
  if ((uint32_t)(now - changedAt) >= 20 && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) { pressedAt = now; handled = false; }
  }
  if (stableState == LOW && !handled && (uint32_t)(now - pressedAt) >= CAL_HOLD_MS) {
    handled = true;
    return true;
  }
  return false;
}
void beginCalibration(const RcInput &rc, const RcInput &mode) {
  if (!groundConfigMode || !rc.valid || !mode.valid || mode.pulseUs > 1300 || normalizeRc(rc.pulseUs) != 0) {
    calibrationStatus = CAL_REJECTED;
    return;
  }
  calibrationActive = true;
  calibrationStatus = CAL_BUSY;
  calSamples = 0;
  calSumDelta = calMinDelta = calMaxDelta = 0;
  resetController(controller);
  canardServo.writeMicroseconds(config.servoMidUs);
}
void sampleCalibration(float rawDeg, const RcInput &rc, const RcInput &mode) {
  if (!groundConfigMode || !rc.valid || !mode.valid || mode.pulseUs > 1300 || normalizeRc(rc.pulseUs) != 0) {
    calibrationActive = false;
    calibrationStatus = CAL_REJECTED;
    return;
  }
  if (!calSamples) calAnchorDeg = rawDeg;
  const float delta = wrap180(rawDeg - calAnchorDeg);
  calMinDelta = min(calMinDelta, delta);
  calMaxDelta = max(calMaxDelta, delta);
  if (calMaxDelta - calMinDelta > CAL_STABILITY_DEG) {
    calibrationActive = false;
    calibrationStatus = CAL_REJECTED;
    return;
  }
  calSumDelta += delta;
  if (++calSamples == CAL_SAMPLE_COUNT) {
    saveCalibration(calAnchorDeg + calSumDelta / CAL_SAMPLE_COUNT);
    calibrationActive = false;
    calibrationStatus = calibrationValid ? CAL_SAVED : CAL_REJECTED;
  }
}

// 一个发送缓冲区，日志拥塞时丢帧并记录计数，绝不等待串口腾空。
void queueEvent(const char *message) {
  const int n = snprintf(telemetry, sizeof(telemetry), "E,%s,%u\n", message, (unsigned)calibrationStatus);
  telemetryLength = n > 0 ? min((size_t)n, sizeof(telemetry) - 1) : 0;
  telemetrySent = 0;
  logResync = true;
}
size_t appendHex(size_t offset, const void *object, size_t length) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(object);
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    telemetry[offset++] = digits[bytes[i] >> 4];
    telemetry[offset++] = digits[bytes[i] & 15];
  }
  return offset;
}
void queueLog(const ControlInput &input, const ControllerMemory &before, const ControlOutput &output) {
  ++logSequence;
  if (telemetrySent < telemetryLength) { if (loggingEnabled) ++logDropped; logResync = true; return; }
  if (metadataPending) {
    memcpy(telemetry, "C,4,", 4);
    size_t n = appendHex(4, &config, sizeof(config));
    telemetry[n++] = ',';
    n = appendHex(n, &sensorZeroDeg, sizeof(sensorZeroDeg));
    telemetry[n++] = '\n';
    telemetryLength = n;
    telemetrySent = 0;
    metadataPending = false;
    logResync = true;
    if (loggingEnabled) ++logDropped;
    return;
  }
  if (!loggingEnabled) return;
  size_t n = 0;
  if (logResync) {
    telemetry[n++] = 'R'; telemetry[n++] = ',';
    n = appendHex(n, &before, sizeof(before));
    telemetry[n++] = '\n';
    logResync = false;
  }
  // 全速记录每个控制输入。R 是丢帧/新会话后的完整算法状态，可精确重同步回放。
  const int count = snprintf(telemetry + n, sizeof(telemetry) - n,
    "D,%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%lu,%lu,%u,%lu\n",
    (unsigned long)logSequence, (unsigned long)input.nowUs, (unsigned long)input.elapsedUs,
    (unsigned)input.aoaAdc, (unsigned)input.rcUs, (unsigned)input.modeUs,
    (unsigned)input.throttleUs, (unsigned)input.flags, (unsigned)output.state,
    (int)(output.commandDeg * 100), (unsigned)output.servoUs,
    (unsigned)(output.effectiveGain * 1000), (unsigned)(controller.assistBlend * 1000),
    (unsigned)(output.effectiveGain > 0 ? (config.throttleSchedule ? 2 : 1) : 0),
    (unsigned long)logDropped, (unsigned long)missedControlPeriods, (unsigned)calibrationStatus,
    (unsigned long)maxControlIntervalUs);
  telemetryLength = n + (count > 0 ? min((size_t)count, sizeof(telemetry) - n - 1) : 0);
  telemetrySent = 0;
}
void flushTelemetry() {
  const int room = Serial.availableForWrite();
  if (room > 0 && telemetrySent < telemetryLength) {
    const size_t count = min((size_t)room, telemetryLength - telemetrySent);
    telemetrySent += Serial.write(reinterpret_cast<const uint8_t *>(telemetry) + telemetrySent, count);
  }
}

bool setParameter(const char *key, float value) {
  if (!isfinite(value)) return false;
  ControlConfig next = config;
  if (KEY_IS("gain")) next.gain = value;
  else if (KEY_IS("authority")) next.authorityDeg = value;
  else if (KEY_IS("limit")) next.limitDeg = value;
  else if (KEY_IS("filter_ms")) next.filterTauUs = value * 1000;
  else if (KEY_IS("slew")) next.slewDegSec = value;
  else if (KEY_IS("throttle_idle")) next.throttleIdleScale = value;
  else if (KEY_IS("throttle_full")) next.throttleFullScale = value;
  else if (KEY_IS("throttle_tau_ms")) next.throttleTauUs = value * 1000;
  else if (KEY_IS("servo_dir") || KEY_IS("rc_dir") || KEY_IS("aoa_dir")) {
    if (value != 1 && value != -1) return false;
    if (KEY_IS("servo_dir")) next.servoDirection = (int8_t)value;
    else if (KEY_IS("rc_dir")) next.rcDirection = (int8_t)value;
    else next.aoaDirection = (int8_t)value;
  } else if (KEY_IS("throttle_schedule")) {
    if (value != 0 && value != 1) return false;
    next.throttleSchedule = (uint8_t)value;
  } else if (KEY_IS("servo_min") || KEY_IS("servo_mid") || KEY_IS("servo_max")) {
    if (value < 500 || value > 2500 || floorf(value) != value) return false;
    if (KEY_IS("servo_min")) next.servoMinUs = (uint16_t)value;
    else if (KEY_IS("servo_mid")) next.servoMidUs = (uint16_t)value;
    else next.servoMaxUs = (uint16_t)value;
  } else return false;
  if (!groundConfigMode || calibrationActive || !validConfig(next)) return false;
  config = next;
  canardServo.attach(PIN_SERVO, config.servoMinUs, config.servoMaxUs);
  canardServo.writeMicroseconds(config.servoMidUs);
  resetController(controller);
  metadataPending = logResync = true;
  return true;
}
void processCommand() {
  if (commandOverflow) { queueEvent("ERR_LONG"); return; }
  char *save = nullptr;
  const char *verb = strtok_r(commandBuffer, " ", &save);
  if (!verb) return;
  const char *key = verb;
  if (KEY_IS("status")) {
    metadataPending = true;
    queueEvent(groundConfigMode ? "GROUND" : "RUN_LOCKED");
    return;
  }
  if (KEY_IS("log")) {
    const char *argument = strtok_r(nullptr, " ", &save);
    if (argument && strcmp(argument, "start") == 0) {
      loggingEnabled = metadataPending = logResync = true;
      queueEvent("LOG_ON");
    } else if (argument && strcmp(argument, "stop") == 0) {
      loggingEnabled = false;
      queueEvent("LOG_OFF");
    } else queueEvent("ERR_ARGUMENT");
    return;
  }
  if (!groundConfigMode) { queueEvent("ERR_LOCKED"); return; }
  if (calibrationActive) { queueEvent("ERR_CAL_BUSY"); return; }
  if (KEY_IS("run")) {
    const RcInput rc = readRcInput(), mode = readChannel(modeCapture);
    if (digitalRead(PIN_GROUND_CONFIG) == LOW || !rc.valid || !mode.valid ||
        mode.pulseUs > 1300 || normalizeRc(rc.pulseUs) != 0) {
      queueEvent("ERR_RUN_INTERLOCK");
      return;
    }
    groundConfigMode = false; // 本次上电内不可重新打开配置状态。
    resetController(controller);
    metadataPending = logResync = true;
    queueEvent("RUN_LOCKED");
  } else if (KEY_IS("save")) queueEvent(saveConfig() ? "SAVED" : "ERR_SAVE");
  else if (KEY_IS("defaults")) {
    config = defaultConfig();
    canardServo.attach(PIN_SERVO, config.servoMinUs, config.servoMaxUs);
    canardServo.writeMicroseconds(config.servoMidUs);
    resetController(controller);
    metadataPending = true;
    queueEvent("DEFAULTS_UNSAVED");
  } else if (KEY_IS("set")) {
    key = strtok_r(nullptr, " ", &save);
    const char *text = strtok_r(nullptr, " ", &save);
    const char *extra = strtok_r(nullptr, " ", &save);
    char *end = nullptr;
    const float value = text ? (float)strtod(text, &end) : NAN;
    if (!key || !text || !end || *end || extra || !setParameter(key, value)) queueEvent("ERR_PARAMETER");
    else queueEvent("SET_UNSAVED");
  } else queueEvent("ERR_COMMAND");
}
void readCommands() {
  if (commandReady) {
    if (telemetrySent >= telemetryLength) {
      processCommand();
      commandReady = false; commandLength = 0; commandOverflow = false;
    }
    return;
  }
  for (uint8_t i = 0; i < 16 && Serial.available() > 0; ++i) {
    const char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      commandBuffer[commandLength] = 0;
      commandReady = true;
      break;
    }
    if (commandLength < sizeof(commandBuffer) - 1) commandBuffer[commandLength++] = ch;
    else commandOverflow = true;
  }
}

void setup() {
  pinMode(PIN_RC_IN, INPUT);
  pinMode(PIN_MODE_IN, INPUT);
  pinMode(PIN_THROTTLE_IN, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);
  pinMode(PIN_CAL_BTN, INPUT_PULLUP);
  pinMode(PIN_GROUND_CONFIG, INPUT_PULLUP);
  groundConfigMode = digitalRead(PIN_GROUND_CONFIG) == LOW;
  loadConfig();
  loadCalibration();
  canardServo.attach(PIN_SERVO, config.servoMinUs, config.servoMaxUs);
  canardServo.writeMicroseconds(config.servoMidUs);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_IN), rcISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_MODE_IN), modeISR, CHANGE);
#ifdef ARDUINO_ARCH_AVR
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    PCMSK2 |= _BV(PCINT23); // D7 = PD7 = PCINT23；仅监听油门输入。
    PCIFR = _BV(PCIF2);
    PCICR |= _BV(PCIE2);
  }
#endif
  Serial.begin(SERIAL_BAUD);
}
void loop() {
  static uint32_t lastControlUs = 0;
  const RcInput rc = readRcInput(), mode = readChannel(modeCapture), throttle = readChannel(throttleCapture);
  if (isCalButtonPressedEvent()) beginCalibration(rc, mode);
  const uint32_t now = micros();
  const uint32_t elapsed = now - lastControlUs;
  if (elapsed >= CONTROL_PERIOD_US) {
    lastControlUs = now;
    maxControlIntervalUs = max(maxControlIntervalUs, elapsed);
    missedControlPeriods += elapsed / CONTROL_PERIOD_US - 1;
    const uint16_t adc = analogRead(PIN_AOA_SENSOR);
    const bool wasCalibrating = calibrationActive;
    if (calibrationActive) sampleCalibration(rawToDeg(adc), rc, mode);
    const uint8_t flags = (rc.valid ? RC_VALID : 0) | (mode.valid ? MODE_VALID : 0) |
      (throttle.valid ? THROTTLE_VALID : 0) | (calibrationValid ? CAL_VALID : 0) |
      (groundConfigMode ? GROUND : 0) | ((wasCalibrating || calibrationActive) ? CAL_BUSY_FLAG : 0);
    const ControlInput input = {now, elapsed, adc, rc.pulseUs, mode.pulseUs, throttle.pulseUs, flags, sensorZeroDeg};
    const ControllerMemory before = controller;
    const ControlOutput output = controlStep(config, controller, input);
    canardServo.writeMicroseconds(output.servoUs);
    queueLog(input, before, output);
  }
  flushTelemetry();
  readCommands();
}
