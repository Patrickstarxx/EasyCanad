#pragma once
#include <array>
#include <cstring>
struct FakeEEPROM {
  std::array<uint8_t, 1024> bytes;
  unsigned writes = 0;
  FakeEEPROM() { bytes.fill(0xff); }
  template<class T> void get(int address, T &value) {
    std::memcpy(&value, bytes.data() + address, sizeof(T));
  }
  template<class T> void put(int address, const T &value) {
    std::memcpy(bytes.data() + address, &value, sizeof(T));
    ++writes;
  }
} EEPROM;
