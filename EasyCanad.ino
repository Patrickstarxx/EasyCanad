#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>

// ===================== 引脚定义 =====================
const uint8_t PIN_AOA_SENSOR = A0;
const uint8_t PIN_SERVO      = 9;
const uint8_t PIN_RC_IN      = 2;
const uint8_t PIN_CAL_BTN    = 4;

// ===================== 参数配置 =====================
float aoaMinDeg = -180.0f;
float aoaMaxDeg =  180.0f;
float aoaOffsetDeg = 0.0f;

const int RC_MIN_US = 1000;
const int RC_MAX_US = 2000;
const int RC_MID_US = 1500;

float rcAuthorityDeg = 90.0f;
float canardLimitDeg = 90.0f;     // 舵机控制范围 -90~+90
float aoaFilterAlpha = 0.15f;
const uint32_t CONTROL_PERIOD_US = 5000;

// ===================== EEPROM标定参数 =====================
const int EEPROM_ADDR_MAGIC = 0;
const int EEPROM_ADDR_ZERO  = EEPROM_ADDR_MAGIC + sizeof(uint32_t);
const uint32_t CAL_MAGIC = 0xA0A0BEEF;
float sensorZeroDeg = 0.0f;

// ===================== RC测量（中断） =====================
volatile uint32_t rcRiseMicros = 0;
volatile uint16_t rcPulseUs = 1500;
volatile bool rcUpdated = false;

Servo canardServo;

// ===================== 工具函数 =====================
static inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// 角度归一化到 [-180, 180)
float wrap180(float a) {
  while (a >= 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

// 计算相对零位后的AOA，并限制到 [-90, 90]
float getAOAdeg(float aoaRawDeg, float sensorZeroDeg) {
  float rel = wrap180(aoaRawDeg - sensorZeroDeg);
  return clampf(rel, -90.0f, 90.0f);
}

// AOA/舵面角度 -> 舵机脉宽
int degToServoUs(float deg) {
  deg = clampf(deg, -90.0f, 90.0f);
  return (int)(mapFloat(deg, -90.0f, 90.0f, 500.0f, 2500.0f) + 0.5f);
}

void rcISR() {
  if (digitalRead(PIN_RC_IN) == HIGH) {
    rcRiseMicros = micros();
  } else {
    uint32_t now = micros();
    uint32_t width = now - rcRiseMicros;
    if (width >= 800 && width <= 2200) {
      rcPulseUs = (uint16_t)width;
      rcUpdated = true;
    }
  }
}

void loadCalibration() {
  uint32_t magic = 0;
  EEPROM.get(EEPROM_ADDR_MAGIC, magic);
  if (magic == CAL_MAGIC) {
    EEPROM.get(EEPROM_ADDR_ZERO, sensorZeroDeg);
    if (isnan(sensorZeroDeg) || sensorZeroDeg < -1000 || sensorZeroDeg > 1000) {
      sensorZeroDeg = 0.0f;
    }
  } else {
    sensorZeroDeg = 0.0f;
  }
}

void saveCalibration(float zeroDeg) {
  EEPROM.put(EEPROM_ADDR_ZERO, zeroDeg);
  uint32_t magic = CAL_MAGIC;
  EEPROM.put(EEPROM_ADDR_MAGIC, magic);
  sensorZeroDeg = zeroDeg;
}

bool isCalButtonPressedEvent() {
  static uint8_t stableState = HIGH;
  static uint8_t lastRead = HIGH;
  static uint32_t tChange = 0;

  uint8_t now = digitalRead(PIN_CAL_BTN);

  if (now != lastRead) {
    lastRead = now;
    tChange = millis();
  }

  if ((millis() - tChange) > 20) {
    if (now != stableState) {
      stableState = now;
      if (stableState == LOW) {
        return true;
      }
    }
  }
  return false;
}

void setup() {
  pinMode(PIN_RC_IN, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);
  pinMode(PIN_CAL_BTN, INPUT_PULLUP);

  canardServo.attach(PIN_SERVO);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_IN), rcISR, CHANGE);

  Serial.begin(115200);
  loadCalibration();
}

void loop() {
  static uint32_t lastControlUs = 0;
  uint32_t nowUs = micros();

  // ========= 标定 =========
  if (isCalButtonPressedEvent()) {
    int rawNow = analogRead(PIN_AOA_SENSOR);
    float voltageNow = rawNow * (5.0f / 1023.0f);
    float aoaRawDegNow = mapFloat(voltageNow, 0.0f, 5.0f, aoaMinDeg, aoaMaxDeg) + aoaOffsetDeg;

    saveCalibration(aoaRawDegNow);
    Serial.print("CAL saved. zero(deg)= ");
    Serial.println(sensorZeroDeg, 3);
  }

  if ((uint32_t)(nowUs - lastControlUs) >= CONTROL_PERIOD_US) {
    lastControlUs = nowUs;

    // ---- 1) 读取传感器 ----
    int raw = analogRead(PIN_AOA_SENSOR);
    float voltage = raw * (5.0f / 1023.0f);
    float aoaRawDeg = mapFloat(voltage, 0.0f, 5.0f, aoaMinDeg, aoaMaxDeg) + aoaOffsetDeg;

    // ---- 2) 零位补偿 + 限幅到 [-90, 90] ----
    float aoaDeg = getAOAdeg(aoaRawDeg, sensorZeroDeg);

    // ---- 3) 滤波 ----
    static float aoaFilt = 0.0f;
    static bool aoaInit = false;
    if (!aoaInit) {
      aoaFilt = aoaDeg;
      aoaInit = true;
    } else {
      aoaFilt = aoaFilt + aoaFilterAlpha * (aoaDeg - aoaFilt);
    }

    // ---- 4) RC输入 ----
    uint16_t rcUsLocal;
    noInterrupts();
    rcUsLocal = rcPulseUs;
    interrupts();

    rcUsLocal = constrain(rcUsLocal, RC_MIN_US, RC_MAX_US);

    float rcNorm;
    if (rcUsLocal >= RC_MID_US) {
      rcNorm = (float)(rcUsLocal - RC_MID_US) / (float)(RC_MAX_US - RC_MID_US);
    } else {
      rcNorm = -(float)(RC_MID_US - rcUsLocal) / (float)(RC_MID_US - RC_MIN_US);
    }
    rcNorm = clampf(rcNorm, -1.0f, 1.0f);
    float rcAddDeg = rcNorm * rcAuthorityDeg;

    // ---- 5) 合成命令，并限制到 [-90, 90] ----
    float canardCmdDeg = -1.2f*aoaFilt - rcAddDeg;
    canardCmdDeg = clampf(canardCmdDeg, -90.0f, 90.0f);

    // ---- 6) 输出舵机 ----
    int servoUs = degToServoUs(canardCmdDeg);
    canardServo.writeMicroseconds(servoUs);

    // 调试输出
    static uint8_t div = 0;
    if (++div >= 20) {
      div = 0;
      Serial.print("AOAraw=");
      Serial.print(aoaRawDeg, 2);
      Serial.print(", Zero=");
      Serial.print(sensorZeroDeg, 2);
      Serial.print(", AOA=");
      Serial.print(aoaFilt, 2);
      Serial.print(", RC=");
      Serial.print(rcUsLocal);
      Serial.print(", Add=");
      Serial.print(rcAddDeg, 2);
      Serial.print(", Cmd=");
      Serial.print(canardCmdDeg, 2);
      Serial.print(", ServoUs=");
      Serial.println(servoUs);
    }
  }
}
