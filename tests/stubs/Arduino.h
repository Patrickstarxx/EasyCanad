#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

const int HIGH = 1, LOW = 0, INPUT = 0, INPUT_PULLUP = 2, CHANGE = 3;
const uint8_t A0 = 14;
extern uint32_t fakeMicros;
extern int fakeAnalog;
extern int fakeRcLevel;
extern int fakeButtonLevel;
inline uint32_t micros() { return fakeMicros; }
inline uint32_t millis() { return fakeMicros / 1000; }
inline void pinMode(uint8_t, int) {}
inline int digitalRead(uint8_t pin) { return pin == 4 ? fakeButtonLevel : fakeRcLevel; }
inline int analogRead(uint8_t) { return fakeAnalog; }
inline int digitalPinToInterrupt(uint8_t pin) { return pin; }
inline void attachInterrupt(int, void (*)(), int) {}
template<class T> inline T min(T a, T b) { return std::min(a, b); }
template<class T> inline T max(T a, T b) { return std::max(a, b); }
template<class T> inline T constrain(T x, T lo, T hi) { return min(max(x, lo), hi); }

struct FakeSerial {
  int room = 63;
  size_t largestWrite = 0;
  std::string output;
  void begin(unsigned long) {}
  int availableForWrite() { return room; }
  size_t write(const uint8_t *bytes, size_t count) {
    if (count > static_cast<size_t>(room)) std::abort();
    largestWrite = std::max(largestWrite, count);
    output.append(reinterpret_cast<const char *>(bytes), count);
    return count;
  }
};
extern FakeSerial Serial;
