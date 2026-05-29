#include <Arduino.h>
#include <Servo.h>

// ===================== 引脚定义 =====================
const uint8_t PIN_AOA_SENSOR = A0;   // 攻角传感器模拟输入 0~5V
const uint8_t PIN_SERVO      = 9;    // 舵机输出（Servo库可用多个引脚）
const uint8_t PIN_RC_IN      = 2;    // 遥控PWM输入，建议D2(INT0)或D3(INT1)

// ===================== 参数配置 =====================
// 传感器映射：0~5V -> -90~+90 deg（按你的风标机械安装可改）
float aoaMinDeg = -180.0f;
float aoaMaxDeg =  180.0f;

// 传感器零偏（安装误差补偿，单位deg）
float aoaOffsetDeg = 0.0f;

// RC输入范围（常见1000~2000us，中位1500us）
const int RC_MIN_US = 1000;
const int RC_MAX_US = 2000;
const int RC_MID_US = 1500;

// RC叠加量：把RC偏移映射成额外舵面角，单位deg
// 例如30表示：1000~2000us -> -30~+30deg
float rcAuthorityDeg = 90.0f;

// 总舵面限幅（鸭翼机械/气动安全限制）单位deg
float canardLimitDeg = 180.0f;

// 一阶低通滤波系数（0~1，越小越平滑）
float aoaFilterAlpha = 0.35f;

// 控制刷新周期（微秒）
const uint32_t CONTROL_PERIOD_US = 5000; // 200Hz

// ===================== RC测量（中断） =====================
volatile uint32_t rcRiseMicros = 0;
volatile uint16_t rcPulseUs = 1500;
volatile bool rcUpdated = false;

void rcISR() {
  // 读取当前电平，计算高电平脉宽
  if (digitalRead(PIN_RC_IN) == HIGH) {
    rcRiseMicros = micros();
  } else {
    uint32_t now = micros();
    uint32_t width = now - rcRiseMicros;
    // 简单有效性判断，防抖/毛刺过滤
    if (width >= 800 && width <= 2200) {
      rcPulseUs = (uint16_t)width;
      rcUpdated = true;
    }
  }
}

// ===================== 全局对象 =====================
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
  // 舵机write角度是0~180，对应我们定义的-90~+90
  float cmd = deg + 90.0f;
  cmd = clampf(cmd, 0.0f, 180.0f);
  return (int)(cmd + 0.5f);
}

// ===================== 初始化 =====================
void setup() {
  pinMode(PIN_RC_IN, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);

  canardServo.attach(PIN_SERVO);  // 默认约50Hz脉冲输出

  // 绑定外部中断，CHANGE沿触发
  attachInterrupt(digitalPinToInterrupt(PIN_RC_IN), rcISR, CHANGE);

  // 可选串口调试
  Serial.begin(115200);
}

// ===================== 主循环（非阻塞） =====================
void loop() {
  static uint32_t lastControlUs = 0;
  uint32_t nowUs = micros();

  if ((uint32_t)(nowUs - lastControlUs) >= CONTROL_PERIOD_US) {
    lastControlUs = nowUs;

    // ---- 1) 读取攻角传感器 ----
    int raw = analogRead(PIN_AOA_SENSOR); // 0~1023
    float voltage = raw * (5.0f / 1023.0f);

    // 0~5V -> aoaMinDeg~aoaMaxDeg
    float aoaDeg = mapFloat(voltage, 0.0f, 5.0f, aoaMinDeg, aoaMaxDeg);
    aoaDeg += aoaOffsetDeg;

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
    //Serial.println(rcUsLocal);
float rcNorm;
if (rcUsLocal >= RC_MID_US) {
  rcNorm = (float)(rcUsLocal - RC_MID_US) / (float)(RC_MAX_US - RC_MID_US); // 0~+1
} else {
  rcNorm = -(float)(RC_MID_US - rcUsLocal) / (float)(RC_MID_US - RC_MIN_US); // -1~0
}
rcNorm = clampf(rcNorm, -1.0f, 1.0f);
float rcAddDeg = rcNorm * rcAuthorityDeg;

    // ---- 3) 合成命令：鸭翼对准来流 + 人工机动叠加 ----
    // 核心：鸭翼角 = 攻角（对准来流） + RC叠加
    float canardCmdDeg = -aoaFilt - rcAddDeg;

    // 总限幅
    canardCmdDeg = clampf(canardCmdDeg, -canardLimitDeg, canardLimitDeg);

    // ---- 4) 输出舵机 ----
    int servoCmd = degToServoCmd(canardCmdDeg);
    canardServo.write(servoCmd);

    // 可选调试输出（降低频率避免阻塞）
    static uint8_t div = 0;
    if (++div >= 20) { // ~10Hz
      div = 0;
      Serial.print("AOA=");
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

  // 这里不使用delay，保持非阻塞
}