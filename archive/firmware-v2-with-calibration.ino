#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>

// ===================== 引脚定义 =====================
const uint8_t PIN_AOA_SENSOR = A0;   // 攻角传感器模拟输入 0~5V
const uint8_t PIN_SERVO      = 9;    // 舵机输出（Servo库可用多个引脚）
const uint8_t PIN_RC_IN      = 2;    // 遥控PWM输入，建议D2(INT0)或D3(INT1)
const uint8_t PIN_CAL_BTN    = 4;    // 标定按键：按下接GND

// ===================== 参数配置 =====================
float aoaMinDeg =  0.0f;
float aoaMaxDeg =  360.0f;

// 传感器额外固定零偏（若不需要可保持0）
float aoaOffsetDeg = 0.0f;

const int RC_MIN_US = 1000;
const int RC_MAX_US = 2000;
const int RC_MID_US = 1500;

float rcAuthorityDeg = 90.0f;
float canardLimitDeg = 180.0f;
float aoaFilterAlpha = 0.15f;
const uint32_t CONTROL_PERIOD_US = 5000; // 200Hz

// ===================== EEPROM标定参数 =====================
const int EEPROM_ADDR_MAGIC = 0;
const int EEPROM_ADDR_ZERO  = EEPROM_ADDR_MAGIC + sizeof(uint32_t);
const uint32_t CAL_MAGIC = 0xA0A0BEEF;   // 用于判断是否有有效标定数据
float sensorZeroDeg = 0.0f;              // EEPROM保存：零攻角对应的原始角度

//float aoaDeg=0;

// ===================== RC测量（中断） =====================
volatile uint32_t rcRiseMicros = 0;
volatile uint16_t rcPulseUs = 1500;
volatile bool rcUpdated = false;

float getAOAdeg(float aoaRawDeg,float sensorZeroDeg)
{
    return aoaRawDeg - sensorZeroDeg;
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

int degToServoCmd(float deg) {
  // 你的当前定义：servo write用0~180，但你逻辑角度用了更大范围
  // 按你原代码保留：deg + 180，再夹到0~360，然后write
  // 注意：Servo.write实际只认0~180，>180会被当180附近处理
  float cmd = deg + 90.0f;
  cmd = clampf(cmd, 0.0f, 180.0f);   // 建议改为180，避免无意义>180
  return (int)(cmd + 0.5f);
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

// 按键消抖 + 边沿检测（非阻塞）
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
        Serial.println("PRESSED");
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
  //Serial.print("Loaded zero(deg)= ");
  //Serial.println(sensorZeroDeg, 3);
}

void loop() {
  static uint32_t lastControlUs = 0;
  uint32_t nowUs = micros();

  // ========= 按键标定处理（随时可按） =========
  if (isCalButtonPressedEvent()) {
    // 读取当前传感器原始角度，记为零攻角
    int rawNow = analogRead(PIN_AOA_SENSOR);
    float voltageNow = rawNow * (5.0f / 1023.0f);
    float aoaRawDegNow = mapFloat(voltageNow, 0.0f, 5.0f, aoaMinDeg, aoaMaxDeg) + aoaOffsetDeg;

    saveCalibration(aoaRawDegNow);

    Serial.print("CAL saved. zero(deg)= ");
    Serial.println(sensorZeroDeg, 3);
  }

  if ((uint32_t)(nowUs - lastControlUs) >= CONTROL_PERIOD_US) {
    lastControlUs = nowUs;

    // ---- 1) 读取攻角传感器 ----
    int raw = analogRead(PIN_AOA_SENSOR);
    float voltage = raw * (5.0f / 1023.0f);

    // 原始角度
    float aoaRawDeg = mapFloat(voltage, 0.0f, 5.0f, aoaMinDeg, aoaMaxDeg);
    aoaRawDeg += aoaOffsetDeg;

    // 应用“零攻角标定”
    float aoaDeg=getAOAdeg(aoaRawDeg,sensorZeroDeg);
    

    // 一阶低通滤波
    static float aoaFilt = 0.0f;
    static bool aoaInit = false;
    if (!aoaInit) {
      aoaFilt = aoaDeg;
      aoaInit = true;
    } else {
      aoaFilt = aoaFilt + aoaFilterAlpha * (aoaDeg - aoaFilt);
    }

    // ---- 2) 读取RC并映射为叠加角 ----
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

    // ---- 3) 合成命令 ----
    float canardCmdDeg = -aoaFilt - rcAddDeg;
    canardCmdDeg = clampf(canardCmdDeg, -canardLimitDeg, canardLimitDeg);

    // ---- 4) 输出舵机 ----
    int servoCmd = degToServoCmd(canardCmdDeg);
    canardServo.write(servoCmd);

    // 调试输出
    static uint8_t div = 0;
    if (++div >= 20) {
      div = 0;
      Serial.print("AOAraw=");
      Serial.print(aoaRawDeg, 2);
      Serial.print(" deg, Zero=");
      Serial.print(sensorZeroDeg, 2);
      Serial.print(" deg, AOA=");
      Serial.print(aoaFilt, 2);
      Serial.print(" deg, RC=");
      Serial.print(rcUsLocal);
      Serial.print(" us, Add=");
      Serial.print(rcAddDeg, 2);
      Serial.print(" deg, Cmd=");
      Serial.print(canardCmdDeg, 2);
      Serial.print(" deg, Servo=");
      Serial.println(servoCmd);
    }
  }
}
