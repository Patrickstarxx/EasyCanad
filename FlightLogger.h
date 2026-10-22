#pragma once
#include "FirmwareState.h"
// 一个发送缓冲区，日志拥塞时丢帧并记录计数，绝不等待串口腾空。
void queueEvent(const char *message) {
  const int n = snprintf(telemetry, sizeof(telemetry), "E,%s,%u\n", message, (unsigned)calibrationStatus);
  telemetryLength = n > 0 ? min((size_t)n, sizeof(telemetry) - 1) : 0;
  telemetrySent = 0;
  logResync = true;
}
size_t appendHex(size_t offset, const void *object, size_t length) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(object);
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    telemetry[offset++] = digits[bytes[i] >> 4];
    telemetry[offset++] = digits[bytes[i] & 15];
  }
  return offset;
}
void queueLog(const ControlInput &input, const ControllerMemory &before, const ControlOutput &output) {
  ++logSequence;
  if (telemetrySent < telemetryLength) { if (loggingEnabled) ++logDropped; logResync = true; return; }
  if (metadataPending) {
    memcpy(telemetry, "C,5,", 4);
    size_t n = appendHex(4, &config, sizeof(config));
    telemetry[n++] = ',';
    n = appendHex(n, &sensorZeroDeg, sizeof(sensorZeroDeg));
    telemetry[n++] = '\n';
    telemetryLength = n;
    telemetrySent = 0;
    metadataPending = false;
    logResync = true;
    if (loggingEnabled) ++logDropped;
    return;
  }
  if (!loggingEnabled) return;
  size_t n = 0;
  if (logResync) {
    telemetry[n++] = 'R'; telemetry[n++] = ',';
    n = appendHex(n, &before, sizeof(before));
    telemetry[n++] = '\n';
    logResync = false;
  }
  // 全速记录每个控制输入。R 是丢帧/新会话后的完整算法状态，可精确重同步回放。
  const int count = snprintf(telemetry + n, sizeof(telemetry) - n,
    "D,%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%lu,%lu,%u,%lu,%lu,%u,%lu\n",
    (unsigned long)logSequence, (unsigned long)input.nowUs, (unsigned long)input.elapsedUs,
    (unsigned)input.aoaAdc, (unsigned)input.rcUs, (unsigned)input.modeUs,
    (unsigned)input.throttleUs, (unsigned)input.flags, (unsigned)output.state,
    (int)(output.commandDeg * 100), (unsigned)output.servoUs,
    (unsigned)(output.effectiveGain * 1000), (unsigned)(controller.assistBlend * 1000),
    (unsigned)(output.effectiveGain > 0 ? (config.throttleSchedule == 2 ? 3 : (config.throttleSchedule ? 2 : 1)) : 0),
    (unsigned long)logDropped, (unsigned long)missedControlPeriods, (unsigned)calibrationStatus,
    (unsigned long)maxControlIntervalUs, (unsigned long)(controller.speed.speedMps * 100),
    (unsigned)controller.speed.status, (unsigned long)controller.speed.ageMs);
  telemetryLength = n + (count > 0 ? min((size_t)count, sizeof(telemetry) - n - 1) : 0);
  telemetrySent = 0;
}
void flushTelemetry() {
  const int room = Serial.availableForWrite();
  if (room > 0 && telemetrySent < telemetryLength) {
    const size_t count = min((size_t)room, telemetryLength - telemetrySent);
    telemetrySent += Serial.write(reinterpret_cast<const uint8_t *>(telemetry) + telemetrySent, count);
  }
}
