#pragma once
#include "FirmwareState.h"
#ifdef ARDUINO_ARCH_AVR
#include <avr/interrupt.h>
#endif
void captureEdge(PwmCapture &channel, bool high, uint32_t now) {
  if (high) { channel.riseUs = now; channel.highSeen = true; }
  else if (channel.highSeen) {
    channel.highSeen = false;
    const uint32_t width = now - channel.riseUs;
    if (width >= 800 && width <= 2200) {
      channel.pulseUs = (uint16_t)width;
      channel.lastUs = now;
      channel.valid = true;
    }
  }
}
void rcISR() { captureEdge(pitchCapture, digitalRead(PIN_RC_IN) == HIGH, micros()); }
void modeISR() { captureEdge(modeCapture, digitalRead(PIN_MODE_IN) == HIGH, micros()); }
void throttleISR() { captureEdge(throttleCapture, digitalRead(PIN_THROTTLE_IN) == HIGH, micros()); }
#ifdef ARDUINO_ARCH_AVR
ISR(PCINT2_vect) { throttleISR(); }
#endif
RcInput readChannel(PwmCapture &channel) {
  RcInput result;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (channel.valid && (uint32_t)(micros() - channel.lastUs) >= RC_TIMEOUT_US) channel.valid = false;
    result.pulseUs = channel.pulseUs;
    result.valid = channel.valid;
  }
  return result;
}
RcInput readRcInput() { return readChannel(pitchCapture); }
