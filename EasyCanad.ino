#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>

// ===================== 引脚定义 =====================
const uint8_t PIN_AOA_SENSOR = A0;
const uint8_t PIN_SERVO      = 9;
const uint8_t PIN_SERVO_REVERSE = 10; // 新增一路：相对 D9 的最终角度指令反向
const uint8_t PIN_RC_IN      = 2;
const uint8_t PIN_CAL_BTN    = 4;

// ===================== 参数配置 =====================
// 迎角补偿比例：建议仅使用 0.0 ~ 1.0；修改后重新编译上传
const float AOA_COMPENSATION_K = 0.5f;
// 飞手中杆、风标处于参考迎角时的鸭翼基础机械配平指令
const float CANARD_TRIM_DEG = 0.0f;
// 相对于 EEPROM 物理零位的参考迎角；这三个参数不写入 EEPROM
const float AOA_REFERENCE_DEG = 0.0f;

float aoaMinDeg = -180.0f;
float aoaMaxDeg =  180.0f;
float aoaOffsetDeg = 0.0f;

const int RC_MIN_US = 1000;
const int RC_MAX_US = 2000;
const int RC_MID_US = 1500;
const uint32_t RC_TIMEOUT_US = 100000; // 无有效脉冲 100 ms 后飞手项归中

float rcAuthorityDeg = 90.0f;
float canardLimitDeg = 90.0f;     // 指令限幅；真实机械行程须台架测量后设定
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
volatile uint32_t rcLastValidUs = 0;
volatile bool rcHavePulse = false;
volatile bool rcRiseSeen = false;

// 在限幅前连续展开角度，避免 +/-180 度边界造成指令换向。
// 相邻采样真实转动须小于 180 度；上电/重新标定后从主值重新开始。
float aoaContinuousDeg = 0.0f;
bool aoaTrackingInit = false;
float aoaFilt = 0.0f;
bool aoaInit = false;

Servo canardServo;
Servo canardServoReverse;

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
  if (!aoaTrackingInit) {
    aoaContinuousDeg = rel;
    aoaTrackingInit = true;
  } else {
    aoaContinuousDeg += wrap180(rel - wrap180(aoaContinuousDeg));
  }
  return clampf(aoaContinuousDeg, -90.0f, 90.0f);
}

// AOA/舵面角度 -> 舵机脉宽
int degToServoUs(float deg) {
  deg = clampf(deg, -90.0f, 90.0f);
  int us = (int)(mapFloat(deg, -90.0f, 90.0f, 500.0f, 2500.0f) + 0.5f);
  // 保持原映射及 attach(pin) 的默认端点；显式反映库原本就会做的截断。
  return constrain(us, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
}

void rcISR() {
  if (digitalRead(PIN_RC_IN) == HIGH) {
    rcRiseMicros = micros();
    rcRiseSeen = true;
  } else {
    uint32_t now = micros();
    uint32_t width = now - rcRiseMicros;
    if (rcRiseSeen && width >= 800 && width <= 2200) {
      rcPulseUs = (uint16_t)width;
      rcLastValidUs = now;
      rcHavePulse = true;
    }
    rcRiseSeen = false;
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
  canardServoReverse.attach(PIN_SERVO_REVERSE);
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
    aoaTrackingInit = false;
    aoaInit = false; // 新零位不混入旧滤波状态
    if (Serial.availableForWrite() >= 40) {
      Serial.print(F("CAL saved. zero(deg)= "));
      Serial.println(sensorZeroDeg, 3);
    }
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
    if (!aoaInit) {
      aoaFilt = aoaDeg;
      aoaInit = true;
    } else {
      aoaFilt = aoaFilt + aoaFilterAlpha * (aoaDeg - aoaFilt);
    }

    // ---- 4) RC输入 ----
    uint16_t rcUsLocal;
    uint32_t rcAgeUs;
    bool rcPulseFresh;
    noInterrupts();
    rcUsLocal = rcPulseUs;
    // 在同一临界区取当前时间，避免 ISR 刚更新的时间戳晚于 nowUs。
    rcAgeUs = (uint32_t)(micros() - rcLastValidUs);
    rcPulseFresh = rcHavePulse && rcAgeUs <= RC_TIMEOUT_US;
    if (!rcPulseFresh) rcHavePulse = false; // 超时锁存，避免计时器整圈后旧值复活
    interrupts();

    // 仅判断脉冲新鲜度；接收机 Failsafe PWM 无法与正常无线连接区分。
    if (!rcPulseFresh) rcUsLocal = RC_MID_US;
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
    // 沿用原方向：正迎角偏差、正 rcAddDeg 均减小鸭翼指令
    float aoaErrorDeg = aoaFilt - AOA_REFERENCE_DEG;
    float canardCmdDeg = CANARD_TRIM_DEG - rcAddDeg
                       - AOA_COMPENSATION_K * aoaErrorDeg;
    float limitDeg = clampf(canardLimitDeg, 0.0f, 90.0f);
    canardCmdDeg = clampf(canardCmdDeg, -limitDeg, limitDeg);

    // ---- 6) 输出舵机 ----
    int servoUs = degToServoUs(canardCmdDeg);
    // 配平、飞手项、补偿合成并限幅后整体取反，两路各自保留原 PWM 端点。
    int servoReverseUs = degToServoUs(-canardCmdDeg);
    canardServo.writeMicroseconds(servoUs);
    canardServoReverse.writeMicroseconds(servoReverseUs);

    // 每 100 ms 取同一帧快照；每个控制周期最多发一个字段。
    // Nano TX 缓冲区可用 >=63 字节才发送，不等待串口排空。
    // 每行一个字段（最长标签+float 文本也小于63字节），15行组成一帧。
    static float debugValues[15];
    static uint8_t debugField = 15;
    static uint8_t div = 0;
    if (div < 20) ++div;
    if (div >= 20 && debugField == 15) {
      div = 0;
      debugValues[0] = aoaRawDeg;
      debugValues[1] = sensorZeroDeg;
      debugValues[2] = aoaFilt;
      debugValues[3] = rcUsLocal;
      debugValues[4] = rcAddDeg;
      debugValues[5] = AOA_COMPENSATION_K;
      debugValues[6] = AOA_REFERENCE_DEG;
      debugValues[7] = aoaErrorDeg;
      debugValues[8] = CANARD_TRIM_DEG;
      debugValues[9] = -rcAddDeg;
      debugValues[10] = -AOA_COMPENSATION_K * aoaErrorDeg;
      debugValues[11] = canardCmdDeg;
      debugValues[12] = servoUs;
      debugValues[13] = servoReverseUs;
      debugValues[14] = rcPulseFresh ? 1.0f : 0.0f;
      debugField = 0;
    }
    if (debugField < 15 && Serial.availableForWrite() >= 63) {
      switch (debugField) {
        case 0: Serial.print(F("AOAraw=")); break;
        case 1: Serial.print(F("Zero=")); break;
        case 2: Serial.print(F("AOA=")); break;
        case 3: Serial.print(F("RC=")); break;
        case 4: Serial.print(F("Add=")); break;
        case 5: Serial.print(F("K=")); break;
        case 6: Serial.print(F("Ref=")); break;
        case 7: Serial.print(F("Err=")); break;
        case 8: Serial.print(F("Trim=")); break;
        case 9: Serial.print(F("Pilot=")); break;
        case 10: Serial.print(F("Auto=")); break;
        case 11: Serial.print(F("Cmd=")); break;
        case 12: Serial.print(F("ServoUs=")); break;
        case 13: Serial.print(F("ServoReverseUs=")); break;
        case 14: Serial.print(F("PWMfresh=")); break;
      }
      uint8_t digits = (debugField == 3 || debugField >= 12) ? 0 : 2;
      Serial.println(debugValues[debugField], digits);
      ++debugField;
    }
  }
}
