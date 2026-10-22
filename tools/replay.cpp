#include "ControlCore.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> fields;
  std::istringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) fields.push_back(field);
  return fields;
}
uint32_t number(const std::string &s, uint32_t maximum = UINT32_MAX) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("invalid unsigned integer");
  const unsigned long long value = std::stoull(s);
  if (value > maximum) throw std::runtime_error("integer outside permitted range");
  return static_cast<uint32_t>(value);
}
int signedNumber(const std::string &s) {
  size_t end = 0;
  const long value = std::stol(s, &end);
  if (end != s.size() || value < -9000 || value > 9000) throw std::runtime_error("invalid command angle");
  return static_cast<int>(value);
}
void decodeHex(const std::string &s, void *object, size_t bytes) {
  if (s.size() != bytes * 2 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw std::runtime_error("invalid snapshot length or hexadecimal value");
  auto *out = static_cast<uint8_t *>(object);
  for (size_t i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(std::stoul(s.substr(i * 2, 2), nullptr, 16));
}
bool validMemory(const ControllerMemory &m) {
  return std::isfinite(m.aoaFilt) && std::fabs(m.aoaFilt) <= 90.001f &&
    std::isfinite(m.previousRawDeg) && std::fabs(m.previousRawDeg) <= 180 &&
    std::isfinite(m.commandDeg) && std::fabs(m.commandDeg) <= 90.001f &&
    std::isfinite(m.assistBlend) && m.assistBlend >= 0 && m.assistBlend <= 1 &&
    std::isfinite(m.throttleNorm) && m.throttleNorm >= 0 && m.throttleNorm <= 1 &&
    m.modeAssistUs <= 100000 && m.sensorGoodSamples <= 10 && m.havePreviousRaw <= 1 && m.haveThrottle <= 1;
}
int main(int argc, char **argv) {
  if (argc < 2) { std::cerr << "Usage: replay LOG [--gain VALUE] [--servo-dir -1|1]\n"; return 2; }
  const uint16_t endian = 1;
  if (*reinterpret_cast<const uint8_t *>(&endian) != 1) { std::cerr << "Little-endian host required\n"; return 2; }
  bool overrideGain = false, overrideDirection = false;
  float gain = 0;
  int direction = 1;
  size_t lineNumber = 0;
  try {
    for (int i = 2; i < argc; i += 2) {
      if (i + 1 >= argc) throw std::runtime_error("missing override value");
      const std::string argument = argv[i];
      size_t end = 0;
      if (argument == "--gain") {
        gain = std::stof(argv[i + 1], &end);
        if (end != std::strlen(argv[i + 1]) || !std::isfinite(gain) || gain < 0 || gain > 5)
          throw std::runtime_error("invalid gain override");
        overrideGain = true;
      } else if (argument == "--servo-dir") {
        direction = std::stoi(argv[i + 1], &end);
        if (end != std::strlen(argv[i + 1]) || (direction != -1 && direction != 1))
          throw std::runtime_error("invalid servo direction override");
        overrideDirection = true;
      } else throw std::runtime_error("unknown override");
    }
    std::ifstream file(argv[1]);
    if (!file) throw std::runtime_error("cannot open log");
    ControlConfig config = defaultConfig();
    ControllerMemory memory = {};
    float zero = 0;
    bool haveConfig = false, haveMemory = false, freshSnapshot = false, haveData = false;
    uint32_t previousSequence = 0;
    unsigned long frames = 0, snapshots = 0, mismatches = 0, missing = 0;
    const bool comparing = !overrideGain && !overrideDirection;
    std::cout << "seq,t_us,dt_us,aoa_adc,rc_us,mode_us,throttle_us,flags,state,cmd_deg,servo_us,effective_gain,assist_blend,gain_source,log_drop,miss,cal_status,max_interval_us,match\n";
    std::string line;
    while (std::getline(file, line)) {
      ++lineNumber;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const auto v = split(line);
      if (v[0] == "E") continue;
      if (v[0] == "C") {
        if (v.size() != 4 || v[1] != "4") throw std::runtime_error("unsupported configuration record");
        decodeHex(v[2], &config, sizeof(config)); decodeHex(v[3], &zero, sizeof(zero));
        if (!validConfig(config) || !std::isfinite(zero) || zero < -180 || zero >= 180)
          throw std::runtime_error("invalid configuration or calibration zero");
        if (overrideGain) config.gain = gain;
        if (overrideDirection) config.servoDirection = static_cast<int8_t>(direction);
        haveConfig = true; haveMemory = false;
      } else if (v[0] == "R") {
        if (!haveConfig || v.size() != 2) throw std::runtime_error("snapshot without configuration");
        decodeHex(v[1], &memory, sizeof(memory));
        if (!validMemory(memory)) throw std::runtime_error("invalid controller snapshot");
        haveMemory = freshSnapshot = true; ++snapshots;
      } else if (v[0] == "D") {
        if (v.size() != 19 || !haveConfig || !haveMemory) throw std::runtime_error("data without complete configuration/snapshot");
        const uint32_t seq = number(v[1]);
        if (haveData && seq - previousSequence != 1) {
          if (!freshSnapshot) throw std::runtime_error("missing or reordered control frame without resynchronization");
          missing += seq - previousSequence - 1;
        }
        ControlInput input = {number(v[2]), number(v[3]), static_cast<uint16_t>(number(v[4], 1023)),
          static_cast<uint16_t>(number(v[5], 2200)), static_cast<uint16_t>(number(v[6], 2200)),
          static_cast<uint16_t>(number(v[7], 2200)), static_cast<uint8_t>(number(v[8], 63)), zero};
        if (input.elapsedUs < 5000) throw std::runtime_error("invalid control period");
        const auto result = controlStep(config, memory, input);
        const unsigned expectedState = number(v[9], THROTTLE_LOST);
        const int expectedCommand = signedNumber(v[10]);
        const unsigned expectedServo = number(v[11], 2500);
        const unsigned expectedGain = number(v[12], 5000), expectedBlend = number(v[13], 1000);
        const unsigned expectedSource = number(v[14], 2);
        const uint32_t drops = number(v[15]), misses = number(v[16]);
        const unsigned calStatus = number(v[17], 3);
        const uint32_t maxInterval = number(v[18]);
        // AVR 与主机浮点舍入允许 1us / 0.02deg 偏差；状态必须一致。
        const bool match = result.state == expectedState &&
          std::abs(static_cast<int>(result.servoUs) - static_cast<int>(expectedServo)) <= 1 &&
          std::fabs(result.commandDeg * 100 - expectedCommand) <= 2 &&
          std::fabs(result.effectiveGain * 1000 - expectedGain) <= 2 &&
          std::fabs(memory.assistBlend * 1000 - expectedBlend) <= 2 &&
          expectedSource == (result.effectiveGain > 0 ? (config.throttleSchedule ? 2U : 1U) : 0U);
        if (comparing && !match) ++mismatches;
        const char *source = result.effectiveGain > 0 ? (config.throttleSchedule ? "throttle_proxy" : "fixed") : "none";
        std::cout << seq << ',' << input.nowUs << ',' << input.elapsedUs << ',' << input.aoaAdc << ','
          << input.rcUs << ',' << input.modeUs << ',' << input.throttleUs << ',' << unsigned(input.flags) << ','
          << unsigned(result.state) << ',' << std::fixed << std::setprecision(5) << result.commandDeg << ','
          << result.servoUs << ',' << result.effectiveGain << ',' << memory.assistBlend << ',' << source << ','
          << drops << ',' << misses << ',' << calStatus << ',' << maxInterval << ','
          << (comparing ? (match ? "1" : "0") : "changed") << '\n';
        ++frames; haveData = true; previousSequence = seq; freshSnapshot = false;
      } else throw std::runtime_error("unknown log record");
    }
    if (!frames) throw std::runtime_error("log has no control frames");
    std::cerr << "Replayed " << frames << " frames; snapshots=" << snapshots << "; missing=" << missing
      << "; mismatches=" << mismatches << (comparing ? "; comparison=on\n" : "; comparison=off (overrides)\n");
    return mismatches ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << "Replay error at line " << lineNumber << ": " << e.what() << '\n'; return 2;
  }
}
