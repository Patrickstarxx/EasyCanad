#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
using std::isnan;
const uint8_t A0 = 14, HIGH = 1, LOW = 0, INPUT = 0, INPUT_PULLUP = 2, CHANGE = 3;
#define F(text) text
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
static uint64_t fakeTime = 10000;
static int pins[20] = {}, pinModes[20] = {}, adc = 512;
static void (*interruptHandler)() = nullptr;
inline uint32_t micros() { return static_cast<uint32_t>(fakeTime); }
inline uint32_t millis() { return static_cast<uint32_t>(fakeTime / 1000); }
inline void pinMode(uint8_t pin, int mode) { pinModes[pin] = mode; }
inline int digitalRead(uint8_t pin) { return pins[pin]; }
inline int analogRead(uint8_t pin) { if (pin != A0) std::abort(); return adc; }
inline int digitalPinToInterrupt(uint8_t pin) { return pin; }
inline void attachInterrupt(int pin, void (*handler)(), int mode) {
  if (pin != 2 || mode != CHANGE) std::abort();
  interruptHandler = handler;
}
inline void noInterrupts() {}
inline void interrupts() {}
struct FakeSerial {
  int capacity = 63;
  unsigned long baud = 0;
  std::string text;
  void begin(unsigned long value) { baud = value; }
  int availableForWrite() const { return capacity; }
  void append(const std::string &value) {
    if (value.size() > static_cast<size_t>(capacity))
      throw std::runtime_error("serial write would block");
    capacity -= static_cast<int>(value.size());
    text += value;
  }
  void print(const char *value) { append(value); }
  void print(float value, int digits = 2) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    append(stream.str());
  }
  void println(float value, int digits = 2) { print(value, digits); append("\r\n"); }
} Serial;
