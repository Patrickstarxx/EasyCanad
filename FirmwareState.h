#pragma once
// 固件共享状态；控制算法不依赖本文件。
#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/atomic.h>
#include "ControlCore.h"

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
const uint16_t CONFIG_VERSION = 6;
struct __attribute__((packed)) LegacyConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint32_t sequence;
  uint8_t configBytes[42];
  uint16_t crc;
};
struct __attribute__((packed)) LegacyV5ConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint32_t sequence;
  uint8_t configBytes[59];
  uint16_t crc;
};
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
