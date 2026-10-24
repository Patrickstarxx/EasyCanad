#pragma once
#include <stdint.h>
#include <math.h>

// 近似水平飞行模型。只有油门和相对攻角，速度没有独立观测校正。
struct __attribute__((packed)) SpeedModelConfig {
  float massGrams;
  float staticThrustGrams;
  float wingAreaDm2;
  float initialSpeedMps;
  uint8_t dragPreset; // 1 低阻、2 中阻、3 高阻；工程假设，不是实测气动系数。
};
enum SpeedModelStatus : uint8_t { SPEED_WAITING, SPEED_PREDICTING, SPEED_INTERRUPTED };
struct __attribute__((packed)) SpeedModelMemory {
  float speedMps;
  uint32_t ageMs;
  uint8_t status;
};

inline bool validSpeedFields(const SpeedModelConfig &c) {
  // 零表示尚未填写，便于在地面逐项配置；完整模型另行要求三项均大于零。
  return isfinite(c.massGrams) && (c.massGrams == 0 || c.massGrams >= 10) && c.massGrams <= 20000 &&
    isfinite(c.staticThrustGrams) && c.staticThrustGrams >= 0 && c.staticThrustGrams <= 50000 &&
    isfinite(c.wingAreaDm2) && (c.wingAreaDm2 == 0 || c.wingAreaDm2 >= 0.1f) && c.wingAreaDm2 <= 500 &&
    isfinite(c.initialSpeedMps) && c.initialSpeedMps >= 0 && c.initialSpeedMps <= 100 &&
    c.dragPreset >= 1 && c.dragPreset <= 3;
}
inline bool validSpeedParameters(const SpeedModelConfig &c) {
  return validSpeedFields(c) && c.massGrams > 0 && c.staticThrustGrams > 0 && c.wingAreaDm2 > 0;
}
inline float modelBaseDrag(uint8_t preset) {
  return preset == 1 ? 0.035f : (preset == 2 ? 0.06f : 0.10f);
}
inline float modelReferenceSpeed(const SpeedModelConfig &c) {
  const float thrustN = c.staticThrustGrams * 0.00980665f;
  const float dragFactor = 0.5f * 1.225f * c.wingAreaDm2 * 0.01f * modelBaseDrag(c.dragPreset);
  return sqrtf(thrustN / dragFactor);
}
inline void updateSpeedModel(const SpeedModelConfig &c, SpeedModelMemory &m,
                             bool mayStart, bool inputsFresh, float throttle,
                             float relativeAoaDeg, uint32_t elapsedUs) {
  if (!validSpeedParameters(c)) return;
  if (m.status == SPEED_WAITING) {
    if (!mayStart || !inputsFresh) return;
    m.speedMps = c.initialSpeedMps;
    m.status = SPEED_PREDICTING;
  }
  if (m.status != SPEED_PREDICTING) return;
  if (!inputsFresh || elapsedUs > 100000) {
    m.status = SPEED_INTERRUPTED; // 中断后不凭空重新赋一个低速度，地面重置后再开始。
    return;
  }
  const float angle = relativeAoaDeg * 0.0174532925f;
  const float sine = sinf(angle);
  const float cd = modelBaseDrag(c.dragPreset) + 0.8f * sine * sine;
  const float massKg = c.massGrams * 0.001f;
  const float projection = fmaxf(0, cosf(angle));
  const float thrustN = c.staticThrustGrams * 0.00980665f * throttle * throttle * projection;
  const float dragPerMass = 0.5f * 1.225f * c.wingAreaDm2 * 0.01f * cd / massKg;
  // 半隐式阻力积分：收油后保留速度并逐渐衰减，不随油门立即归零。
  uint32_t remaining = elapsedUs;
  while (remaining) {
    const uint32_t stepUs = remaining > 10000 ? 10000 : remaining;
    const float dt = stepUs * 0.000001f;
    m.speedMps = (m.speedMps + thrustN / massKg * dt) / (1 + dragPerMass * m.speedMps * dt);
    remaining -= stepUs;
  }
  m.ageMs += elapsedUs / 1000;
}
