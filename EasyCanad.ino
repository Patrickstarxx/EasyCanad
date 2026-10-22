#include "FirmwareState.h"
#include "PwmInput.h"
#include "EepromStorage.h"
#include "ConfigStore.h"
#include "CalibrationStore.h"
#include "FlightLogger.h"
#include "SerialConsole.h"

void setup() {
  pinMode(PIN_RC_IN, INPUT);
  pinMode(PIN_MODE_IN, INPUT);
  pinMode(PIN_THROTTLE_IN, INPUT);
  pinMode(PIN_AOA_SENSOR, INPUT);
  pinMode(PIN_CAL_BTN, INPUT_PULLUP);
  pinMode(PIN_GROUND_CONFIG, INPUT_PULLUP);
  groundConfigMode = digitalRead(PIN_GROUND_CONFIG) == LOW;
  loadConfig();
  loadCalibration();
  canardServo.attach(PIN_SERVO, config.servoMinUs, config.servoMaxUs);
  canardServo.writeMicroseconds(config.servoMidUs);
  attachInterrupt(digitalPinToInterrupt(PIN_RC_IN), rcISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_MODE_IN), modeISR, CHANGE);
#ifdef ARDUINO_ARCH_AVR
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    PCMSK2 |= _BV(PCINT23); // D7 = PD7 = PCINT23；仅监听油门输入。
    PCIFR = _BV(PCIF2);
    PCICR |= _BV(PCIE2);
  }
#endif
  Serial.begin(SERIAL_BAUD);
}
void loop() {
  static uint32_t lastControlUs = 0;
  const RcInput rc = readRcInput(), mode = readChannel(modeCapture), throttle = readChannel(throttleCapture);
  if (isCalButtonPressedEvent()) beginCalibration(rc, mode);
  const uint32_t now = micros();
  const uint32_t elapsed = now - lastControlUs;
  if (elapsed >= CONTROL_PERIOD_US) {
    lastControlUs = now;
    maxControlIntervalUs = max(maxControlIntervalUs, elapsed);
    missedControlPeriods += elapsed / CONTROL_PERIOD_US - 1;
    const uint16_t adc = analogRead(PIN_AOA_SENSOR);
    const bool wasCalibrating = calibrationActive;
    if (calibrationActive) sampleCalibration(rawToDeg(adc), rc, mode);
    const uint8_t flags = (rc.valid ? RC_VALID : 0) | (mode.valid ? MODE_VALID : 0) |
      (throttle.valid ? THROTTLE_VALID : 0) | (calibrationValid ? CAL_VALID : 0) |
      (groundConfigMode ? GROUND : 0) | ((wasCalibrating || calibrationActive) ? CAL_BUSY_FLAG : 0);
    const ControlInput input = {now, elapsed, adc, rc.pulseUs, mode.pulseUs, throttle.pulseUs, flags, sensorZeroDeg};
    const ControllerMemory before = controller;
    const ControlOutput output = controlStep(config, controller, input);
    canardServo.writeMicroseconds(output.servoUs);
    queueLog(input, before, output);
  }
  flushTelemetry();
  readCommands();
}
