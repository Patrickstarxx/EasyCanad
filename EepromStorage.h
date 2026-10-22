#pragma once
#include "FirmwareState.h"
uint16_t crc16(const uint8_t *bytes, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= (uint16_t)bytes[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}
void writeCommittedRecord(int address, const uint8_t *bytes, size_t length, uint32_t magic) {
  const uint32_t invalid = 0;
  EEPROM.put(address, invalid);
  for (size_t i = sizeof(magic); i < length; ++i) EEPROM.update(address + i, bytes[i]);
  EEPROM.put(address, magic);
}
