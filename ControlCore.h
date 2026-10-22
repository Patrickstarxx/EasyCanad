#pragma once
#include <stdint.h>
#include <math.h>

// 此文件同时用于机载控制和离线回放，不依赖 Arduino 或硬件。
struct __attribute__((packed)) ControlConfig {
  float gain;
  float authorityDeg;
  float limitDeg;
  float filterTauUs;
  float slewDegSec;
  float throttleIdleScale;
  float throttleFullScale;
  float throttleTauUs;
  uint16_t servoMinUs;
  uint16_t servoMidUs;
  uint16_t servoMaxUs;
  int8_t servoDirection;
  int8_t rcDirection;
  int8_t aoaDirection;
  uint8_t throttleSchedule;
};

enum ControlState : uint8_t {
  UNCALIBRATED, RC_LOST, CALIBRATING, SENSOR_FAULT, ACTIVE,
  MANUAL, MODE_LOST, GROUND_CONFIG, THROTTLE_LOST
};
enum InputFlags : uint8_t {
  RC_VALID = 1, MODE_VALID = 2, THROTTLE_VALID = 4,
  CAL_VALID = 8, GROUND = 16, CAL_BUSY_FLAG = 32
};
struct ControlInput {
  uint32_t nowUs;
  uint32_t elapsedUs;
  uint16_t aoaAdc;
  uint16_t rcUs;
  uint16_t modeUs;
  uint16_t throttleUs;
  uint8_t flags;
  float zeroDeg;
};
struct __attribute__((packed)) ControllerMemory {
  float aoaFilt;
  float previousRawDeg;
  float commandDeg;
  float assistBlend;
  float throttleNorm;
  uint32_t modeAssistUs;
  uint8_t sensorGoodSamples;
  uint8_t havePreviousRaw;
  uint8_t haveThrottle;
};
struct ControlOutput {
  ControlState state;
  float commandDeg;
  float manualDeg;
  float automaticDeg;
  float effectiveGain;
  uint16_t servoUs;
};

static_assert(sizeof(float) == 4 && sizeof(ControlConfig) == 42 && sizeof(ControllerMemory) == 27,
              "Log format requires packed 32-bit floats");

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float wrap180(float a) {
  if (!isfinite(a)) return 0;
  while (a >= 180) a -= 360;
  while (a < -180) a += 360;
  return a;
}
inline float rawToDeg(int raw) { return wrap180(-180.0f + raw * (360.0f / 1023.0f)); }
inline float normalizeRc(uint16_t width) {
  const float delta = clampf(width, 1000, 2000) - 1500;
  if (fabsf(delta) <= 10) return 0;
  return delta > 0 ? (delta - 10) / 490 : (delta + 10) / 490;
}
inline ControlConfig defaultConfig() {
  // 油门只是调度代理：默认最多把辅助增益降至一半，不计算或声称测得空速。
  ControlConfig c = {1.2f, 90, 90, 28333.333f, 360, 1, 0.5f, 500000,
                     500, 1500, 2500, 1, 1, 1, 1};
  return c;
}
inline bool validConfig(const ControlConfig &c) {
  return isfinite(c.gain) && c.gain >= 0 && c.gain <= 5 &&
    isfinite(c.authorityDeg) && c.authorityDeg >= 0 && c.authorityDeg <= 90 &&
    isfinite(c.limitDeg) && c.limitDeg > 0 && c.limitDeg <= 90 &&
    isfinite(c.filterTauUs) && c.filterTauUs >= 1000 && c.filterTauUs <= 1000000 &&
    isfinite(c.slewDegSec) && c.slewDegSec >= 10 && c.slewDegSec <= 1000 &&
    isfinite(c.throttleIdleScale) && c.throttleIdleScale >= 0 && c.throttleIdleScale <= 1 &&
    isfinite(c.throttleFullScale) && c.throttleFullScale >= 0 && c.throttleFullScale <= 1 &&
    c.throttleFullScale <= c.throttleIdleScale &&
    isfinite(c.throttleTauUs) && c.throttleTauUs >= 10000 && c.throttleTauUs <= 5000000 &&
    c.servoMinUs >= 500 && c.servoMaxUs <= 2500 &&
    c.servoMinUs < c.servoMidUs && c.servoMidUs < c.servoMaxUs &&
    c.servoMinUs % 4 == 0 && c.servoMaxUs % 4 == 0 &&
    (c.servoDirection == 1 || c.servoDirection == -1) &&
    (c.rcDirection == 1 || c.rcDirection == -1) &&
    (c.aoaDirection == 1 || c.aoaDirection == -1) && c.throttleSchedule <= 1;
}
inline void resetController(ControllerMemory &m) { m = ControllerMemory{}; }
inline int degToServoUs(const ControlConfig &c, float deg) {
  deg = clampf(deg, -c.limitDeg, c.limitDeg) * c.servoDirection;
  const float span = deg >= 0 ? c.servoMaxUs - c.servoMidUs : c.servoMidUs - c.servoMinUs;
  return (int)(c.servoMidUs + deg * span / 90.0f + 0.5f);
}
inline ControlOutput controlStep(const ControlConfig &c, ControllerMemory &m, const ControlInput &in) {
  ControlOutput out = {MANUAL, 0, 0, 0, 0, c.servoMidUs};
  if (in.flags & (GROUND | CAL_BUSY_FLAG)) {
    out.state = in.flags & CAL_BUSY_FLAG ? CALIBRATING : GROUND_CONFIG;
    resetController(m);
    return out;
  }
  if (!(in.flags & RC_VALID)) { out.state = RC_LOST; resetController(m); return out; }

  const float raw = rawToDeg(in.aoaAdc);
  const float relative = wrap180(raw - in.zeroDeg);
  const bool plausible = isfinite(in.zeroDeg) && fabsf(relative) <= 90 &&
    (!m.havePreviousRaw || fabsf(wrap180(raw - m.previousRawDeg)) <= 45);
  m.previousRawDeg = raw;
  m.havePreviousRaw = 1;
  if (!plausible) m.sensorGoodSamples = 0;
  else {
    if (!m.sensorGoodSamples) m.aoaFilt = relative;
    else m.aoaFilt += in.elapsedUs / (c.filterTauUs + in.elapsedUs) * (relative - m.aoaFilt);
    if (m.sensorGoodSamples < 10) ++m.sensorGoodSamples;
  }
  const uint32_t boundedDt = in.elapsedUs < 50000 ? in.elapsedUs : 50000;
  const bool modeValid = in.flags & MODE_VALID;
  const bool requestAssist = modeValid && in.modeUs >= 1700;
  if (!requestAssist) m.modeAssistUs = 0;
  else if (m.modeAssistUs < 100000) {
    // 一次长暂停不能代替连续有效的开关输入。
    m.modeAssistUs += in.elapsedUs < 5000 ? in.elapsedUs : 5000;
  }
  if (in.flags & THROTTLE_VALID) {
    const float target = clampf((in.throttleUs - 1000.0f) / 1000, 0, 1);
    if (!m.haveThrottle) { m.throttleNorm = target; m.haveThrottle = 1; }
    else m.throttleNorm += boundedDt / (c.throttleTauUs + boundedDt) * (target - m.throttleNorm);
  } else m.haveThrottle = 0;

  const float rc = normalizeRc(in.rcUs) * c.rcDirection;
  out.manualDeg = clampf(-rc * c.authorityDeg, -c.limitDeg, c.limitDeg);
  bool assistAllowed = false;
  if (!modeValid) out.state = MODE_LOST;
  else if (!requestAssist || m.modeAssistUs < 100000) out.state = MANUAL;
  else if (!(in.flags & CAL_VALID)) out.state = UNCALIBRATED;
  else if (m.sensorGoodSamples < 10) out.state = SENSOR_FAULT;
  else if (c.throttleSchedule && !(in.flags & THROTTLE_VALID)) out.state = THROTTLE_LOST;
  else { out.state = ACTIVE; assistAllowed = true; }

  const float blendStep = boundedDt / 300000.0f;
  if (assistAllowed) m.assistBlend = clampf(m.assistBlend + blendStep, 0, 1);
  else if (out.state == MANUAL) m.assistBlend = clampf(m.assistBlend - blendStep, 0, 1);
  else m.assistBlend = 0;
  // 手动切换时允许补偿平滑退出；传感器/通道故障则立即撤掉补偿目标。
  const bool fading = out.state == MANUAL && (in.flags & CAL_VALID) && m.sensorGoodSamples >= 10 &&
    (!c.throttleSchedule || (in.flags & THROTTLE_VALID));
  if (assistAllowed || fading) {
    const float scale = c.throttleSchedule ?
      c.throttleIdleScale + (c.throttleFullScale - c.throttleIdleScale) * m.throttleNorm : 1;
    out.effectiveGain = c.gain * scale * m.assistBlend;
    const float remaining = c.limitDeg - fabsf(out.manualDeg);
    out.automaticDeg = clampf(-out.effectiveGain * c.aoaDirection * m.aoaFilt, -remaining, remaining);
  }
  const float target = clampf(out.manualDeg + out.automaticDeg, -c.limitDeg, c.limitDeg);
  const float step = c.slewDegSec * boundedDt / 1000000.0f;
  m.commandDeg = clampf(target, m.commandDeg - step, m.commandDeg + step);
  out.commandDeg = m.commandDeg;
  out.servoUs = degToServoUs(c, out.commandDeg);
  return out;
}
