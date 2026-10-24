#pragma once
#include <algorithm>
class Servo {
public:
  int minUs = 544, maxUs = 2400, lastUs = 1500;
  uint8_t attach(int, int minimum = 544, int maximum = 2400) {
    minUs = minimum;
    maxUs = maximum;
    return 0;
  }
  void writeMicroseconds(int value) { lastUs = std::max(minUs, std::min(maxUs, value)); }
};
