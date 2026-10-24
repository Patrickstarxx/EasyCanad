#pragma once
#include "PwmInput.h"
#include "ConfigStore.h"
#include "CalibrationStore.h"
#include "FlightLogger.h"
#ifdef ARDUINO_ARCH_AVR
#include <avr/pgmspace.h>
#define KEY_IS(name) (strcmp_P(key, PSTR(name)) == 0)
#else
#define KEY_IS(name) (strcmp(key, name) == 0)
#endif
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
  else if (KEY_IS("model_mass_g")) next.speedModel.massGrams = value;
  else if (KEY_IS("model_thrust_g")) next.speedModel.staticThrustGrams = value;
  else if (KEY_IS("model_area_dm2")) next.speedModel.wingAreaDm2 = value;
  else if (KEY_IS("model_initial_mps")) next.speedModel.initialSpeedMps = value;
  else if (KEY_IS("trim_deg")) next.trimDeg = value;
  else if (KEY_IS("aoa_reference_deg")) next.aoaReferenceDeg = value;
  else if (KEY_IS("compensation_ratio")) next.compensationRatio = value;
  else if (KEY_IS("law_mode")) {
    if (value != 0 && value != 1) return false;
    next.lawMode = (uint8_t)value;
  }
  else if (KEY_IS("model_drag")) {
    if (value != 1 && value != 2 && value != 3) return false;
    next.speedModel.dragPreset = (uint8_t)value;
  }
  else if (KEY_IS("servo_dir") || KEY_IS("rc_dir") || KEY_IS("aoa_dir")) {
    if (value != 1 && value != -1) return false;
    if (KEY_IS("servo_dir")) next.servoDirection = (int8_t)value;
    else if (KEY_IS("rc_dir")) next.rcDirection = (int8_t)value;
    else next.aoaDirection = (int8_t)value;
  } else if (KEY_IS("throttle_schedule")) {
    if (value != 0 && value != 1 && value != 2) return false;
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


#undef KEY_IS
