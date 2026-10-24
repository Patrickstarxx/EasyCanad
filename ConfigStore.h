#pragma once
#include "EepromStorage.h"
bool readConfigSlot(uint8_t slot, ConfigRecord &record) {
  EEPROM.get(CONFIG_SLOTS[slot], record);
  if (record.magic == CONFIG_MAGIC && record.version == 4) {
    LegacyConfigRecord old;
    EEPROM.get(CONFIG_SLOTS[slot], old);
    if (old.crc != crc16(reinterpret_cast<const uint8_t *>(&old), offsetof(LegacyConfigRecord, crc))) return false;
    record.config = defaultConfig();
    memcpy(&record.config, old.configBytes, sizeof(old.configBytes));
    record.sequence = old.sequence;
    return validConfig(record.config);
  }
  if (record.magic == CONFIG_MAGIC && record.version == 5) {
    LegacyV5ConfigRecord old;
    EEPROM.get(CONFIG_SLOTS[slot], old);
    if (old.crc != crc16(reinterpret_cast<const uint8_t *>(&old), offsetof(LegacyV5ConfigRecord, crc))) return false;
    record.config = defaultConfig();
    memcpy(&record.config, old.configBytes, sizeof(old.configBytes));
    record.sequence = old.sequence;
    return validConfig(record.config);
  }
  return record.magic == CONFIG_MAGIC && record.version == CONFIG_VERSION &&
    record.crc == crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(ConfigRecord, crc)) &&
    validConfig(record.config);
}
void loadConfig() {
  ConfigRecord first, second;
  const bool a = readConfigSlot(0, first), b = readConfigSlot(1, second);
  if (!a && !b) { config = defaultConfig(); activeConfigSlot = -1; configSequence = 0; return; }
  const bool useSecond = b && (!a || (int32_t)(second.sequence - first.sequence) > 0);
  activeConfigSlot = useSecond ? 1 : 0;
  const ConfigRecord &record = useSecond ? second : first;
  config = record.config;
  configSequence = record.sequence;
}
bool saveConfig() {
  if (!groundConfigMode || calibrationActive || !validConfig(config)) return false;
  const uint8_t slot = activeConfigSlot == 0 ? 1 : 0;
  ConfigRecord record = {CONFIG_MAGIC, CONFIG_VERSION, configSequence + 1, config, 0};
  record.crc = crc16(reinterpret_cast<const uint8_t *>(&record), offsetof(ConfigRecord, crc));
  writeCommittedRecord(CONFIG_SLOTS[slot], reinterpret_cast<const uint8_t *>(&record), sizeof(record), record.magic);
  ConfigRecord verified;
  if (!readConfigSlot(slot, verified) || verified.sequence != record.sequence) return false;
  activeConfigSlot = slot;
  configSequence = record.sequence;
  return true;
}
