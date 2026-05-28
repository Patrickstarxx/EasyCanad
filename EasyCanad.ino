#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>

// ===================== 引脚定义 =====================
const uint8_t PIN_AOA_SENSOR = A0;
const uint8_t PIN_SERVO_L    = 9;    // 左鸭翼舵机
const uint8_t PIN_SERVO_R    = 10;   // 右鸭翼舵机
const uint8_t PIN_RC_PIT     = 2;    // 俯仰(升降)PWM输入 (INT0)
const uint8_t PIN_RC_AIL     = 3;    // 副翼PWM输入 (INT1)
const uint8_t PIN_CAL_BTN    = 4;

// ===================== 参数配置 =====================
float aoaMinDeg = -180.0f;
float aoaMaxDeg =  180.0f;
float aoaOffsetDeg = 0.0f;

const int RC_MIN_US = 1000;
const int RC_MAX_US = 2000;
const int RC_MID_US = 1500;

float rcAuthorityDeg = 90.0f;      // 俯仰RC权限
float rollAuthorityDeg = 45.0f;    // 副翼混控滚转权限(deg)
float canardLimitDeg = 90.0f;      // 舵机控制范围 -90~+90
float aoaFilterAlpha = 0.15f;
const uint32_t CONTROL_PERIOD_US = 5000;

// ===================== EEPROM标定参数 =====================
const int EEPROM_ADDR_MAGIC = 0;
const int EEPROM_ADDR_ZERO  = EEPROM_ADDR_MAGIC + sizeof(uint32_t);
const uint32_t CAL_MAGIC = 0xA0A0BEEF;
float sensorZeroDeg = 0.0f;

// ===================== RC测量（中断） =====================
// 俯仰通道
volatile uint32_t rcPitRiseMicros = 0;
volatile uint16_t rcPitPulseUs = 1500;
volatile bool rcPitUpdated = false;

// 副翼通道
volatile uint32_t rcAilRiseMicros = 0;
volatile uint16_t rcAilPulseUs = 1500;
volatile bool rcAilUpdated = false;

Servo canardServoL;  // 左鸭翼
Servo canardServoR;  // 右鸭翼

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

// ===================== RC中断服务 =====================
void rcPitISR() {
  if (digitalRead(PIN_RC_PIT) == HIGH) {
    rcPitRiseMicros = micros();
  } else {
    uint32_t now = micros();
    uint32_t width = now - rcPitRiseMicros;
    if (width >= 800 && width <= 2200) {
      rcPitPulseUs = (uint16_t)width;
      rcPitUpdated = true;
    }
  }
}

void rcAilISR() {
  if (digitalRead(PIN_RC_AIL) == HIGH) {
    rcAilRiseMicros = micros();
  } else {
    uint32_t now = micros();
    uint32_t width = now - rcAilRiseMicros;
    if (width >= 800 && width <= 2200) {
      rcAilPulseUs = (uint16_t)width;
      rcAilUpdated = true;
    }
  }
}

// 将RC脉宽映射为归一化值 [-1, +1]
float rcUsToNorm(uint16_t us) {
  us = constrain(us, RC_MIN_US, RC_MAX_US);
  if (us >= RC_MID_US) {
    return (float)(us - RC_MID_US) / (float)(RC_MAX_US - RC_MID_US);
  } else {
    return -(float)(RC_MID_US - us) / (float)(RC_MID_US - RC_MIN_US);
  }
}

// ===================== 标定相关 =====================
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
  pinMode(PIN_RC_PIT, INPUT);
  pinMode(PIN_RC_AIL, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);
  pinMode(PIN_CAL_BTN, INPUT_PULLUP);

  canardServoL.attach(PIN_SERVO_L);
  canardServoR.attach(PIN_SERVO_R);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_PIT), rcPitISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_AIL), rcAilISR, CHANGE);

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

    // ---- 4a) 俯仰RC输入 ----
    uint16_t pitUsLocal;
    noInterrupts();
    pitUsLocal = rcPitPulseUs;
    interrupts();
    float pitNorm = rcUsToNorm(pitUsLocal);
    pitNorm = clampf(pitNorm, -1.0f, 1.0f);
    float pitAddDeg = pitNorm * rcAuthorityDeg;

    // ---- 4b) 副翼RC输入 ----
    uint16_t ailUsLocal;
    noInterrupts();
    ailUsLocal = rcAilPulseUs;
    interrupts();
    float ailNorm = rcUsToNorm(ailUsLocal);
    ailNorm = clampf(ailNorm, -1.0f, 1.0f);
    float ailAddDeg = ailNorm * rollAuthorityDeg;

    // ---- 5) 俯仰/副翼混控 ----
    // 俯仰分量: AOA稳定 + 升降杆 -> 两鸭翼同向偏转
    float pitchCmd = -1.2f * aoaFilt - pitAddDeg;
    // 滚转分量: 副翼杆 -> 两鸭翼差动 (左+ 右-)
    float rollCmd  = ailAddDeg;

    float leftCanardDeg  = pitchCmd + rollCmd;
    float rightCanardDeg = pitchCmd - rollCmd;

    leftCanardDeg  = clampf(leftCanardDeg,  -canardLimitDeg, canardLimitDeg);
    rightCanardDeg = clampf(rightCanardDeg, -canardLimitDeg, canardLimitDeg);

    // ---- 6) 输出舵机 ----
    int servoUsL = degToServoUs(leftCanardDeg);
    int servoUsR = degToServoUs(rightCanardDeg);
    canardServoL.writeMicroseconds(servoUsL);
    canardServoR.writeMicroseconds(servoUsR);

    // 调试输出
    static uint8_t div = 0;
    if (++div >= 20) {
      div = 0;
      Serial.print("AOA=");
      Serial.print(aoaFilt, 2);
      Serial.print(", Pit=");
      Serial.print(pitUsLocal);
      Serial.print("(");
      Serial.print(pitAddDeg, 1);
      Serial.print("), Ail=");
      Serial.print(ailUsLocal);
      Serial.print("(");
      Serial.print(ailAddDeg, 1);
      Serial.print("), L=");
      Serial.print(leftCanardDeg, 1);
      Serial.print("(");
      Serial.print(servoUsL);
      Serial.print("), R=");
      Serial.print(rightCanardDeg, 1);
      Serial.print("(");
      Serial.print(servoUsR);
      Serial.println(")");
    }
  }
}
