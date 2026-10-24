#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
struct FakeEEPROM {
  std::array<uint8_t, 1024> data;
  int failAfter = -1;
  int changedWrites = 0;
  FakeEEPROM() { data.fill(0xFF); }
  template<class T> void get(int address, T &value) {
    std::memcpy(&value, data.data() + address, sizeof(T));
  }
  void update(int address, uint8_t value) {
    if (data.at(address) == value) return;
    if (failAfter >= 0 && changedWrites == failAfter) throw std::runtime_error("power loss");
    ++changedWrites;
    data.at(address) = value;
  }
  template<class T> void put(int address, const T &value) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) update(address + static_cast<int>(i), bytes[i]);
  }
};
extern FakeEEPROM EEPROM;
