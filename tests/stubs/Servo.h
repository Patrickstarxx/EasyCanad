#pragma once
const int MIN_PULSE_WIDTH = 544, MAX_PULSE_WIDTH = 2400;
struct Servo {
  int pin = -1, pulse = 1500, writes = 0;
  void attach(int value) { pin = value; }
  void writeMicroseconds(int value) {
    // Match the default AVR Servo software limits, not a physical servo model.
    pulse = constrain(value, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
    ++writes;
  }
};
