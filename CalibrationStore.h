#pragma once
#include "EepromStorage.h"
uint16_t calibrationCrc(const CalibrationRecord &record) {
  return crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(CalibrationRecord, crc));
}
void loadCalibration() {
  CalibrationRecord record;
  EEPROM.get(EEPROM_CAL_ADDR, record);
  calibrationValid = record.magic == CAL_MAGIC && record.version == CAL_VERSION &&
    record.crc == calibrationCrc(record) && isfinite(record.zeroDeg) &&
    record.zeroDeg >= -180 && record.zeroDeg < 180 &&
    record.minDeg == -180 && record.maxDeg == 180 && record.offsetDeg == 0;
  sensorZeroDeg = calibrationValid ? record.zeroDeg : 0;
  resetController(controller);
  metadataPending = logResync = true;
}
void saveCalibration(float zeroDeg) {
  CalibrationRecord record = {CAL_MAGIC, CAL_VERSION, wrap180(zeroDeg), -180, 180, 0, 0};
  record.crc = calibrationCrc(record);
  writeCommittedRecord(EEPROM_CAL_ADDR, reinterpret_cast<const uint8_t *>(&record), sizeof(record), record.magic);
  loadCalibration();
}
bool isCalButtonPressedEvent() {
  static uint8_t lastRead = HIGH, stableState = HIGH;
  static uint32_t changedAt = 0, pressedAt = 0;
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
void beginCalibration(const RcInput &rc, const RcInput &mode) {
  if (!groundConfigMode || !rc.valid || !mode.valid || mode.pulseUs > 1300 || normalizeRc(rc.pulseUs) != 0) {
    calibrationStatus = CAL_REJECTED;
    return;
  }
  calibrationActive = true;
  calibrationStatus = CAL_BUSY;
  calSamples = 0;
  calSumDelta = calMinDelta = calMaxDelta = 0;
  resetController(controller);
  canardServo.writeMicroseconds(config.servoMidUs);
}
void sampleCalibration(float rawDeg, const RcInput &rc, const RcInput &mode) {
  if (!groundConfigMode || !rc.valid || !mode.valid || mode.pulseUs > 1300 || normalizeRc(rc.pulseUs) != 0) {
    calibrationActive = false;
    calibrationStatus = CAL_REJECTED;
    return;
  }
  if (!calSamples) calAnchorDeg = rawDeg;
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
