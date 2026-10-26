#include <iostream>
#include "firmware_under_test.ino"

static void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
static void near(float actual, float expected, const char *message) {
  check(std::fabs(actual - expected) < 0.001f, message);
}
static void boot() {
  pins[4] = HIGH;
  setup();
  check(Serial.baud == 115200 && canardServo.pin == 9, "setup pins/baud");
  check(pinModes[4] == INPUT_PULLUP && interruptHandler == rcISR, "button/ISR setup");
  check(EEPROM.writes == 0, "startup must not erase or migrate EEPROM");
}
static void pulseAt(uint64_t end, uint32_t width) {
  fakeTime = end - width; pins[2] = HIGH; rcISR();
  fakeTime = end; pins[2] = LOW; rcISR();
}
static void setAngle(float angle) {
  // Keep the real ADC conversion in loop, offset the test fixture to exact angles.
  aoaOffsetDeg = angle - mapFloat(adc * (5.0f / 1023.0f), 0, 5, aoaMinDeg, aoaMaxDeg);
}
static void tick(float angle, int pwm = 1500, int capacity = 63) {
  const uint64_t next = fakeTime + CONTROL_PERIOD_US;
  if (pwm >= 0) pulseAt(next, static_cast<uint32_t>(pwm));
  else fakeTime = next;
  setAngle(angle);
  Serial.capacity = capacity;
  loop();
}
static int expectedPWM(float command) {
  const float limit = std::fmax(0.0f, std::fmin(90.0f, canardLimitDeg));
  command = std::fmax(-limit, std::fmin(limit, command));
  const int requested = static_cast<int>(1500.0f + command * (1000.0f / 90.0f) + 0.5f);
  return std::max(544, std::min(2400, requested));
}
static float command(float angle, float pilotAdd = 0) {
  return CANARD_TRIM_DEG - pilotAdd - AOA_COMPENSATION_K * (angle - AOA_REFERENCE_DEG);
}
static void outputIs(float target) {
  // Check the real loop output, allowing only the specified integer PWM rounding.
  check(canardServo.pulse == expectedPWM(target), "actual sketch PWM mismatch");
}
static void resetAngle() { aoaTrackingInit = false; aoaInit = false; }

static void mathCase() {
  aoaFilterAlpha = 1;
  const float angles[] = {0, 5, 10, 20, -10};
  const float half[] = {0, -2.5f, -5, -10, 5};
  const float full[] = {0, -5, -10, -20, 10};
  for (unsigned i = 0; i < 5; ++i) {
    resetAngle(); tick(angles[i]);
    float autoAngle = AOA_COMPENSATION_K == 0 ? 0 :
                      (AOA_COMPENSATION_K == 1 ? full[i] : half[i]);
    const float expectedCommand = CANARD_TRIM_DEG + autoAngle + AOA_COMPENSATION_K * AOA_REFERENCE_DEG;
    outputIs(expectedCommand);
    // Drain any preceding snapshot, then verify the exact command before PWM rounding.
    for (int n = 0; n < 40; ++n) tick(angles[i]);
    Serial.text.clear();
    for (int n = 0; n < 40; ++n) tick(angles[i]);
    std::ostringstream expectedLine;
    expectedLine << "Cmd=" << std::fixed << std::setprecision(2) << expectedCommand << "\r\n";
    check(Serial.text.find(expectedLine.str()) != std::string::npos, "table command telemetry");
  }
  resetAngle(); tick(AOA_REFERENCE_DEG);
  outputIs(CANARD_TRIM_DEG);
  // Check exact mathematical command in telemetry too, before PWM quantization.
  Serial.text.clear();
  for (int i = 0; i < 60; ++i) tick(AOA_REFERENCE_DEG);
  std::ostringstream expected;
  expected << "Cmd=" << std::fixed << std::setprecision(2) << CANARD_TRIM_DEG << "\r\n";
  check(Serial.text.find(expected.str()) != std::string::npos, "reference/trim command telemetry");
  // The original first-order filter coefficient remains functional.
  resetAngle(); tick(0); aoaFilterAlpha = 0.15f; tick(10);
  near(aoaFilt, 1.5f, "filter coefficient preserved");
}
static void mappingCase() {
  aoaFilterAlpha = 1;
  const int widths[] = {800, 1000, 1250, 1500, 1750, 2000, 2200};
  const float adds[] = {-90, -90, -45, 0, 45, 90, 90};
  for (unsigned i = 0; i < 7; ++i) {
    tick(AOA_REFERENCE_DEG, widths[i]);
    outputIs(CANARD_TRIM_DEG - adds[i]);
  }
  rcAuthorityDeg = 20;
  tick(AOA_REFERENCE_DEG, 1750); outputIs(CANARD_TRIM_DEG - 10);
  check(degToServoUs(-90) == 544 && degToServoUs(90) == 2400, "Servo defaults");
  check(degToServoUs(0) == 1500 && degToServoUs(-45) == 1000 && degToServoUs(45) == 2000,
        "legacy PWM direction and scale");
}
static void limitsCase() {
  aoaFilterAlpha = 1;
  for (float limit : {0.0f, 20.0f, 90.0f, 120.0f, -1.0f}) {
    canardLimitDeg = limit;
    for (float angle : {-90.0f, -20.0f, 20.0f, 90.0f}) {
      for (int pwm : {1000, 1250, 1750, 2000}) {
        resetAngle(); tick(angle, pwm);
        outputIs(command(angle, (pwm - 1500) * 90.0f / 500.0f));
        const float bounded = std::max(0.0f, std::min(90.0f, limit));
        check(canardServo.pulse >= expectedPWM(-bounded) &&
              canardServo.pulse <= expectedPWM(bounded), "mechanical command limit");
      }
    }
  }
}
static void wrapCase() {
  resetAngle();
  near(getAOAdeg(179, 179), 0, "zero at sensor seam");
  near(getAOAdeg(-179, 179), 2, "positive seam delta");
  near(getAOAdeg(178, 179), -1, "reverse seam delta");
  resetAngle();
  for (float angle : {170.0f, 179.0f, -179.0f, -170.0f, -179.0f, 179.0f})
    near(getAOAdeg(angle, 0), 90, "positive saturation must not flip at antipode");
  resetAngle();
  for (float angle : {-170.0f, -179.0f, 179.0f, 170.0f, 179.0f, -179.0f})
    near(getAOAdeg(angle, 0), -90, "negative saturation must not flip at antipode");
  resetAngle(); aoaFilterAlpha = 1;
  for (float angle : {170.0f, 179.0f, -179.0f, -170.0f}) {
    tick(angle); near(aoaFilt, 90, "real loop seam"); outputIs(command(90));
  }
  resetAngle(); sensorZeroDeg = 179;
  tick(179); tick(-179); near(aoaFilt, 2, "real loop calibrated seam");
}
static void calibrationCase() {
  // Pre-existing main EEPROM binary layout, not new parameters.
  check(EEPROM_ADDR_MAGIC == 0 && EEPROM_ADDR_ZERO == 4 && CAL_MAGIC == 0xA0A0BEEF,
        "EEPROM layout unchanged");
  const uint32_t magic = 0xA0A0BEEF;
  const float zero = 12.25f;
  EEPROM.put(0, magic); EEPROM.put(4, zero);
  const auto before = EEPROM.bytes;
  const unsigned writes = EEPROM.writes;
  loadCalibration(); near(sensorZeroDeg, zero, "legacy zero load");
  aoaFilterAlpha = 1;
  for (int i = 0; i < 40; ++i) tick(zero + AOA_REFERENCE_DEG);
  outputIs(CANARD_TRIM_DEG);
  check(EEPROM.bytes == before && EEPROM.writes == writes, "K/trim/reference must not write EEPROM");
  pins[4] = LOW; tick(30);
  pins[4] = HIGH; tick(30); // bounce
  pins[4] = LOW;
  for (int i = 0; i < 5; ++i) tick(30);
  check(EEPROM.writes == writes, "button debounce");
  tick(30);
  near(sensorZeroDeg, 30, "D4 saves physical raw angle");
  near(aoaFilt, 0, "calibration resets continuous/filter state");
  for (int i = 0; i < 30; ++i) tick(30);
  check(EEPROM.writes == writes + 2, "held button only saves once");
  sensorZeroDeg = -50; loadCalibration(); near(sensorZeroDeg, 30, "reboot zero reload");
  for (size_t i = 8; i < EEPROM.bytes.size(); ++i)
    check(EEPROM.bytes[i] == before[i], "no new EEPROM parameters");
}
static void rcCase() {
  aoaFilterAlpha = 1;
  tick(10, -1); outputIs(command(10)); check(!rcHavePulse, "startup has no pulse");
  // A falling edge without a rising edge cannot establish input validity.
  rcRiseMicros = micros() - 1500; pins[2] = LOW; rcISR();
  check(!rcHavePulse, "unpaired falling edge rejected");
  tick(10, 1750); outputIs(command(10, 45));
  for (int i = 0; i < 20; ++i) tick(10, -1);
  outputIs(command(10, 45)); // exactly 100 ms is still accepted
  tick(10, -1); outputIs(command(10)); check(!rcHavePulse, "timeout latched");
  for (int width : {799, 2201, 5000}) {
    tick(10, width); outputIs(command(10)); check(!rcHavePulse, "invalid width rejected");
  }
  tick(10, 1250); outputIs(command(10, -45));
  for (int i = 0; i < 22; ++i) tick(10, 799);
  outputIs(command(10)); check(!rcHavePulse, "invalid pulses must not refresh last good input");
  tick(10, 1750);
  pins[2] = HIGH; rcISR(); // stuck high / missing falling edge
  for (int i = 0; i < 22; ++i) tick(10, -1);
  outputIs(command(10));
  // Receiver-generated valid failsafe PWM is indistinguishable from live RC PWM.
  for (int i = 0; i < 100; ++i) tick(10, 1250);
  outputIs(command(10, -45)); check(rcHavePulse, "valid failsafe PWM remains pulse-present");
}
static void rolloverCase() {
  aoaFilterAlpha = 1;
  pulseAt((uint64_t(1) << 32) + 700, 1750); // rising/falling edges straddle micros wrap
  check(rcPulseUs == 1750 && rcHavePulse, "pulse width across rollover");
  tick(0, -1); outputIs(command(0, 45));
  for (int i = 0; i < 20; ++i) tick(0, -1);
  outputIs(command(0)); check(!rcHavePulse, "timeout across rollover");
  fakeTime += (uint64_t(1) << 32) - 105000;
  tick(0, -1); outputIs(command(0)); check(!rcHavePulse, "stale value cannot revive after full wrap");
}
static void telemetryCase() {
  aoaFilterAlpha = 1;
  for (int i = 0; i < 60; ++i) tick(10, 1600);
  for (const char *label : {"AOAraw=", "Zero=", "AOA=", "RC=", "Add=", "K=", "Ref=",
                            "Err=", "Trim=", "Pilot=", "Auto=", "Cmd=", "ServoUs=", "PWMfresh=1"})
    check(Serial.text.find(label) != std::string::npos, "missing telemetry field");
  const std::string before = Serial.text;
  const int writes = canardServo.writes;
  for (int i = 0; i < 300; ++i) tick(-10, -1, 0);
  check(Serial.text == before && canardServo.writes == writes + 300, "TX backpressure must not stop control");
  outputIs(command(-10));
  for (int i = 0; i < 60; ++i) tick(0, -1);
  check(Serial.text.find("PWMfresh=0") != std::string::npos, "pulse timeout telemetry");
  const int count = canardServo.writes;
  ++fakeTime; Serial.capacity = 63; loop();
  check(canardServo.writes == count, "control period remains 5000 us");
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "scenario required"); boot();
    const std::string name = argv[1];
    if (name == "math") mathCase();
    else if (name == "mapping") mappingCase();
    else if (name == "limits") limitsCase();
    else if (name == "wrap") wrapCase();
    else if (name == "calibration") calibrationCase();
    else if (name == "rc") rcCase();
    else if (name == "rollover") rolloverCase();
    else if (name == "telemetry") telemetryCase();
    else check(false, "unknown scenario");
  } catch (const std::exception &error) {
    std::cerr << (argc > 1 ? argv[1] : "test") << ": " << error.what() << '\n';
    return 1;
  }
}
