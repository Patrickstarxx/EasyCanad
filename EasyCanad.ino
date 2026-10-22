#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <util/atomic.h>

// ===================== 引脚和安装参数 =====================
const uint8_t PIN_AOA_SENSOR = A0;
const uint8_t PIN_SERVO = 9;
const uint8_t PIN_RC_IN = 2;
const uint8_t PIN_CAL_BTN = 4;

const float aoaMinDeg = -180.0f;
const float aoaMaxDeg = 180.0f;
const float aoaOffsetDeg = 0.0f;
const float aoaGain = 1.2f;          // 控制方向须结合实际安装验证
const float rcAuthorityDeg = 90.0f;
constexpr float canardLimitDeg = 90.0f;  // 实际舵面机械限幅，不是舵机行程
const float AOA_VALID_LIMIT_DEG = 90.0f;
const float AOA_MAX_STEP_DEG = 45.0f; // 每次采样的跳变门限，需台架验证
const uint8_t SENSOR_RECOVERY_SAMPLES = 10;
const float AOA_FILTER_TAU_US = 28333.333f; // 5ms 周期时 alpha = 0.15
const float COMMAND_RATE_DEG_PER_SEC = 360.0f;

const int RC_MIN_US = 1000;
const int RC_MID_US = 1500;
const int RC_MAX_US = 2000;
const int RC_DEADBAND_US = 10;
const uint32_t RC_TIMEOUT_US = 100000;
// 按实际舵机、连杆校准；attach 和角度映射共用这些端点。
const int SERVO_MIN_US = 500;
const int SERVO_MID_US = 1500;
const int SERVO_MAX_US = 2500;
const uint32_t CONTROL_PERIOD_US = 5000;
const uint32_t TELEMETRY_PERIOD_US = 100000;
const uint32_t CAL_HOLD_MS = 1500;
const uint8_t CAL_SAMPLE_COUNT = 32;
const float CAL_STABILITY_DEG = 2.0f;

static_assert(SERVO_MIN_US < SERVO_MID_US && SERVO_MID_US < SERVO_MAX_US,
              "Servo endpoints must surround neutral");
static_assert(canardLimitDeg > 0 && canardLimitDeg <= 90,
              "Canard limit must be in (0, 90]");

// 新格式不读取旧版 0..7 地址：旧标定没有记录角度坐标系，无法可靠迁移。
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

enum ControlState : uint8_t { UNCALIBRATED, RC_LOST, CALIBRATING, SENSOR_FAULT, ACTIVE };
enum CalibrationStatus : uint8_t { CAL_IDLE, CAL_BUSY, CAL_SAVED, CAL_REJECTED };
struct RcInput { uint16_t pulseUs; bool valid; };

// 显式声明依赖自定义类型的函数，避免 Arduino 自动原型被插入到类型定义之前。
RcInput readRcInput();
uint16_t calibrationCrc(const CalibrationRecord &record);
void beginCalibration(const RcInput &rc);
void sampleCalibration(float rawDeg, const RcInput &rc);
void queueTelemetry(ControlState state, const RcInput &rc, float command, int servoUs);

volatile uint32_t rcRiseMicros = 0;
volatile uint32_t rcLastValidMicros = 0;
volatile uint16_t rcPulseUs = RC_MID_US;
volatile bool rcHighSeen = false;
volatile bool rcHasSignal = false;
Servo canardServo;
float sensorZeroDeg = 0;
bool calibrationValid = false;
bool calibrationActive = false;
CalibrationStatus calibrationStatus = CAL_IDLE;
uint8_t calSamples = 0;
float calAnchorDeg = 0;
float calSumDelta = 0;
float calMinDelta = 0;
float calMaxDelta = 0;
float aoaFilt = 0;
float previousRawDeg = 0;
bool havePreviousRaw = false;
uint8_t sensorGoodSamples = 0;
float lastCommandDeg = 0;
uint32_t missedControlPeriods = 0;
uint32_t maxControlIntervalUs = 0;
char telemetry[144];
size_t telemetryLength = 0;
size_t telemetrySent = 0;

static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

float wrap180(float a) {
  if (!isfinite(a)) return 0;
  while (a >= 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

float rawToDeg(int raw) {
  return wrap180(aoaMinDeg + raw * ((aoaMaxDeg - aoaMinDeg) / 1023.0f) + aoaOffsetDeg);
}

int degToServoUs(float deg) {
  deg = clampf(deg, -canardLimitDeg, canardLimitDeg);
  // 舵面逻辑角度仍按 +/-90 映射，不因减小机械限幅而重新放大行程。
  const float span = deg >= 0 ? SERVO_MAX_US - SERVO_MID_US : SERVO_MID_US - SERVO_MIN_US;
  return (int)(SERVO_MID_US + deg * span / 90.0f + 0.5f);
}

float normalizeRc(uint16_t pulseUs) {
  const int value = constrain((int)pulseUs, RC_MIN_US, RC_MAX_US);
  const int delta = value - RC_MID_US;
  if (abs(delta) <= RC_DEADBAND_US) return 0;
  if (delta > 0) return (float)(delta - RC_DEADBAND_US) / (RC_MAX_US - RC_MID_US - RC_DEADBAND_US);
  return (float)(delta + RC_DEADBAND_US) / (RC_MID_US - RC_MIN_US - RC_DEADBAND_US);
}

void rcISR() {
  const uint32_t now = micros();
  if (digitalRead(PIN_RC_IN) == HIGH) {
    rcRiseMicros = now;
    rcHighSeen = true;
  } else if (rcHighSeen) {
    rcHighSeen = false;
    const uint32_t width = now - rcRiseMicros;
    if (width >= 800 && width <= 2200) {
      rcPulseUs = (uint16_t)width;
      rcLastValidMicros = now;
      rcHasSignal = true;
    }
  }
}

RcInput readRcInput() {
  RcInput input;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    // 在同一原子区读取时间，避免中断刚更新的时间戳比主循环快照还新。
    if (rcHasSignal && (uint32_t)(micros() - rcLastValidMicros) >= RC_TIMEOUT_US) {
      rcHasSignal = false; // 锁定丢失状态，直到有新脉冲；计时器再回绕也不会复活旧数据。
    }
    input.pulseUs = rcPulseUs;
    input.valid = rcHasSignal;
  }
  return input;
}

uint16_t calibrationCrc(const CalibrationRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < offsetof(CalibrationRecord, crc); ++i) {
    crc ^= (uint16_t)bytes[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  return crc;
}

void resetSensorFilter() {
  aoaFilt = 0;
  sensorGoodSamples = 0;
  havePreviousRaw = false;
}

void loadCalibration() {
  CalibrationRecord record;
  EEPROM.get(EEPROM_CAL_ADDR, record);
  calibrationValid = record.magic == CAL_MAGIC && record.version == CAL_VERSION &&
    record.crc == calibrationCrc(record) && isfinite(record.zeroDeg) &&
    record.zeroDeg >= -180.0f && record.zeroDeg < 180.0f &&
    record.minDeg == aoaMinDeg && record.maxDeg == aoaMaxDeg && record.offsetDeg == aoaOffsetDeg;
  sensorZeroDeg = calibrationValid ? record.zeroDeg : 0;
  resetSensorFilter();
}

void saveCalibration(float zeroDeg) {
  CalibrationRecord record = { CAL_MAGIC, CAL_VERSION, wrap180(zeroDeg), aoaMinDeg,
                               aoaMaxDeg, aoaOffsetDeg, 0 };
  record.crc = calibrationCrc(record);
  // 标定期间舵机保持中位；先使记录失效，最后写 magic 提交，断电不会接受半条记录。
  const uint32_t invalidMagic = 0;
  EEPROM.put(EEPROM_CAL_ADDR, invalidMagic);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  for (size_t i = sizeof(record.magic); i < sizeof(record); ++i) {
    EEPROM.update(EEPROM_CAL_ADDR + i, bytes[i]);
  }
  EEPROM.put(EEPROM_CAL_ADDR, record.magic);
  loadCalibration(); // 回读、校验并清除旧滤波状态
}

bool isCalButtonPressedEvent() {
  static uint8_t lastRead = HIGH;
  static uint8_t stableState = HIGH;
  static uint32_t changedAt = 0;
  static uint32_t pressedAt = 0;
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

void beginCalibration(const RcInput &rc) {
  if (!rc.valid || normalizeRc(rc.pulseUs) != 0) {
    calibrationStatus = CAL_REJECTED;
    return;
  }
  calibrationActive = true;
  calibrationStatus = CAL_BUSY;
  calSamples = 0;
  calSumDelta = calMinDelta = calMaxDelta = 0;
  lastCommandDeg = 0;
  canardServo.writeMicroseconds(SERVO_MID_US);
}

void sampleCalibration(float rawDeg, const RcInput &rc) {
  if (!rc.valid || normalizeRc(rc.pulseUs) != 0) {
    calibrationActive = false;
    calibrationStatus = CAL_REJECTED;
    return;
  }
  if (calSamples == 0) calAnchorDeg = rawDeg;
  // 以首样本展开小范围角度，179/-179 度附近的样本不会平均成 0 度。
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

bool updateSensor(float rawDeg, uint32_t elapsedUs) {
  const float relative = wrap180(rawDeg - sensorZeroDeg);
  const bool plausible = fabsf(relative) <= AOA_VALID_LIMIT_DEG &&
    (!havePreviousRaw || fabsf(wrap180(rawDeg - previousRawDeg)) <= AOA_MAX_STEP_DEG);
  previousRawDeg = rawDeg;
  havePreviousRaw = true;
  if (!plausible) {
    sensorGoodSamples = 0;
    return false;
  }
  if (sensorGoodSamples == 0) aoaFilt = relative;
  else {
    const float alpha = elapsedUs / (AOA_FILTER_TAU_US + elapsedUs);
    aoaFilt += alpha * (relative - aoaFilt);
  }
  if (sensorGoodSamples < SENSOR_RECOVERY_SAMPLES) ++sensorGoodSamples;
  return sensorGoodSamples >= SENSOR_RECOVERY_SAMPLES;
}

float mixedCommand(float rcNorm, float filteredAoa) {
  const float manual = clampf(-rcNorm * rcAuthorityDeg, -canardLimitDeg, canardLimitDeg);
  const float remaining = canardLimitDeg - fabsf(manual);
  const float automatic = clampf(-aoaGain * filteredAoa, -remaining, remaining);
  return clampf(manual + automatic, -canardLimitDeg, canardLimitDeg);
}

void queueTelemetry(ControlState state, const RcInput &rc, float command, int servoUs) {
  static uint32_t lastTelemetryUs = 0;
  const uint32_t now = micros();
  if ((uint32_t)(now - lastTelemetryUs) < TELEMETRY_PERIOD_US || telemetrySent < telemetryLength) return;
  lastTelemetryUs = now;
  // AVR 默认 printf 不支持浮点格式；角度以十分之一度输出，分块发送避免堵住控制循环。
  const int count = snprintf(telemetry, sizeof(telemetry),
    "State=%u,AOA10=%d,RC=%u,Cmd10=%d,ServoUs=%d,Cal=%u,Miss=%lu,MaxUs=%lu\n",
    (unsigned)state, (int)(aoaFilt * 10), (unsigned)rc.pulseUs, (int)(command * 10),
    servoUs, (unsigned)calibrationStatus, (unsigned long)missedControlPeriods,
    (unsigned long)maxControlIntervalUs);
  telemetryLength = count > 0 ? min((size_t)count, sizeof(telemetry) - 1) : 0;
  telemetrySent = 0;
}

void flushTelemetry() {
  const int room = Serial.availableForWrite();
  if (room > 0 && telemetrySent < telemetryLength) {
    const size_t count = min((size_t)room, telemetryLength - telemetrySent);
    telemetrySent += Serial.write(reinterpret_cast<const uint8_t *>(telemetry) + telemetrySent, count);
  }
}

void setup() {
  pinMode(PIN_RC_IN, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);
  pinMode(PIN_CAL_BTN, INPUT_PULLUP);
  canardServo.attach(PIN_SERVO, SERVO_MIN_US, SERVO_MAX_US);
  canardServo.writeMicroseconds(SERVO_MID_US);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_IN), rcISR, CHANGE);
  Serial.begin(115200);
  loadCalibration();
}

void loop() {
  static uint32_t lastControlUs = 0;
  const RcInput rc = readRcInput();
  if (isCalButtonPressedEvent()) beginCalibration(rc);
  const uint32_t now = micros();
  const uint32_t elapsedUs = now - lastControlUs;
  if (elapsedUs >= CONTROL_PERIOD_US) {
    lastControlUs = now;
    maxControlIntervalUs = max(maxControlIntervalUs, elapsedUs);
    missedControlPeriods += elapsedUs / CONTROL_PERIOD_US - 1;
    const float rawDeg = rawToDeg(analogRead(PIN_AOA_SENSOR));
    const bool wasCalibrating = calibrationActive;
    if (calibrationActive) sampleCalibration(rawDeg, rc);
    const bool sensorHealthy = updateSensor(rawDeg, elapsedUs);
    ControlState state;
    // 完成或中断标定的这个周期也保持中位；EEPROM 写入不造成恢复瞬间跳舵。
    if (wasCalibrating || calibrationActive) state = CALIBRATING;
    else if (!rc.valid) state = RC_LOST;
    else if (!calibrationValid) state = UNCALIBRATED;
    else if (!sensorHealthy) state = SENSOR_FAULT;
    else state = ACTIVE;

    float command = 0;
    if (state == ACTIVE || state == SENSOR_FAULT) {
      const float rcNorm = normalizeRc(rc.pulseUs);
      const float target = state == ACTIVE ? mixedCommand(rcNorm, aoaFilt) :
        clampf(-rcNorm * rcAuthorityDeg, -canardLimitDeg, canardLimitDeg);
      const float maxStep = COMMAND_RATE_DEG_PER_SEC * min(elapsedUs, (uint32_t)50000) / 1000000.0f;
      command = clampf(target, lastCommandDeg - maxStep, lastCommandDeg + maxStep);
    }
    lastCommandDeg = command;
    const int servoUs = degToServoUs(command);
    canardServo.writeMicroseconds(servoUs);
    queueTelemetry(state, rc, command, servoUs);
  }
  flushTelemetry();
}
