import { decodeConfigLine, type ConfigSnapshot } from './config';

export type DataFrame = {
  sequence: number; timeUs: number; elapsedUs: number; aoaAdc: number;
  rcUs: number; modeUs: number; throttleUs: number; flags: number; state: number;
  commandDeg: number; servoUs: number; effectiveGain: number; assistBlend: number;
  gainSource: number; dropped: number; missed: number; calibrationStatus: number;
  maxIntervalUs: number; modelSpeedMps: number; modelStatus: number; modelAgeMs: number;
};
export type ParsedLog = { config: ConfigSnapshot; frames: DataFrame[]; raw: string; gaps: number };
export const STATE_NAMES = [
  '未标定', '俯仰失联', '标定中', '风标异常', '辅助有效', '手动',
  '模式失联', '地面配置', '油门失联', '模型不可用',
];

function integer(value: string, min: number, max: number): number {
  if (!/^\d+$/.test(value)) throw new Error('日志整数格式错误');
  const n = Number(value);
  if (!Number.isSafeInteger(n) || n < min || n > max) throw new Error('日志数值超出范围');
  return n;
}
function signedInteger(value: string, min: number, max: number): number {
  if (!/^-?\d+$/.test(value)) throw new Error('日志整数格式错误');
  const n = Number(value);
  if (!Number.isSafeInteger(n) || n < min || n > max) throw new Error('日志数值超出范围');
  return n;
}

export function decodeDataLine(line: string, version: number): DataFrame {
  const v = line.trim().split(',');
  if (v[0] !== 'D' || v.length !== (version === 4 ? 19 : 22)) throw new Error('数据帧长度错误');
  const u = (index: number, max = 0xffffffff) => integer(v[index], 0, max);
  return {
    sequence: u(1), timeUs: u(2), elapsedUs: u(3), aoaAdc: u(4, 1023),
    rcUs: u(5, 2200), modeUs: u(6, 2200), throttleUs: u(7, 2200), flags: u(8, 63),
    state: u(9, 9), commandDeg: signedInteger(v[10], -9000, 9000) / 100,
    servoUs: u(11, 2500), effectiveGain: u(12, 5000) / 1000,
    assistBlend: u(13, 1000) / 1000, gainSource: u(14, version === 6 ? 4 : 3),
    dropped: u(15), missed: u(16), calibrationStatus: u(17, 3), maxIntervalUs: u(18),
    modelSpeedMps: version >= 5 ? u(19) / 100 : 0,
    modelStatus: version >= 5 ? u(20, 2) : 0,
    modelAgeMs: version >= 5 ? u(21) : 0,
  };
}

export function parseLog(raw: string): ParsedLog {
  let config: ConfigSnapshot | undefined;
  const frames: DataFrame[] = [];
  let gaps = 0;
  for (const line of raw.split(/\r?\n/)) {
    if (!line || line.startsWith('#') || line.startsWith('E,') || line.startsWith('R,')) continue;
    if (line.startsWith('C,')) { config = decodeConfigLine(line); continue; }
    if (line.startsWith('D,')) {
      if (!config) throw new Error('日志缺少配置记录');
      const frame = decodeDataLine(line, config.version);
      const previous = frames.at(-1);
      if (previous && frame.sequence > previous.sequence + 1) gaps += frame.sequence - previous.sequence - 1;
      frames.push(frame);
      continue;
    }
    throw new Error('日志包含未知记录');
  }
  if (!config || !frames.length) throw new Error('日志缺少配置或数据帧');
  return { config, frames, raw, gaps };
}

export function framesToCsv(frames: DataFrame[]): string {
  const headers: (keyof DataFrame)[] = [
    'sequence', 'timeUs', 'elapsedUs', 'aoaAdc', 'rcUs', 'modeUs', 'throttleUs',
    'flags', 'state', 'commandDeg', 'servoUs', 'effectiveGain', 'assistBlend',
    'gainSource', 'dropped', 'missed', 'calibrationStatus', 'maxIntervalUs',
    'modelSpeedMps', 'modelStatus', 'modelAgeMs',
  ];
  return [headers.join(','), ...frames.map(frame => headers.map(key => frame[key]).join(','))].join('\n') + '\n';
}
