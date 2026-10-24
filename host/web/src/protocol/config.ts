export type ControlConfig = {
  gain: number; authorityDeg: number; limitDeg: number; filterTauUs: number; slewDegSec: number;
  throttleIdleScale: number; throttleFullScale: number; throttleTauUs: number;
  servoMinUs: number; servoMidUs: number; servoMaxUs: number;
  servoDirection: number; rcDirection: number; aoaDirection: number; throttleSchedule: number;
  modelMassGrams: number; modelStaticThrustGrams: number; modelWingAreaDm2: number;
  modelInitialSpeedMps: number; modelDragPreset: number;
  trimDeg: number; aoaReferenceDeg: number; compensationRatio: number; lawMode: number;
};
export type ConfigKey = keyof ControlConfig;
export type ConfigSnapshot = { version: 4 | 5 | 6; zeroDeg: number; config: ControlConfig };

export const DEFAULT_CONFIG: ControlConfig = {
  gain: 1.2, authorityDeg: 90, limitDeg: 90, filterTauUs: 28333.333,
  slewDegSec: 360, throttleIdleScale: 1, throttleFullScale: 0.5, throttleTauUs: 500000,
  servoMinUs: 500, servoMidUs: 1500, servoMaxUs: 2500,
  servoDirection: 1, rcDirection: 1, aoaDirection: 1, throttleSchedule: 0,
  modelMassGrams: 0, modelStaticThrustGrams: 0, modelWingAreaDm2: 0,
  modelInitialSpeedMps: 0, modelDragPreset: 2,
  trimDeg: 0, aoaReferenceDeg: 0, compensationRatio: 0, lawMode: 0,
};

export const CONFIG_KEYS = Object.keys(DEFAULT_CONFIG) as ConfigKey[];
const FLOAT_KEYS = new Set<ConfigKey>([
  'gain', 'authorityDeg', 'limitDeg', 'filterTauUs', 'slewDegSec', 'throttleIdleScale',
  'throttleFullScale', 'throttleTauUs', 'modelMassGrams', 'modelStaticThrustGrams',
  'modelWingAreaDm2', 'modelInitialSpeedMps', 'trimDeg', 'aoaReferenceDeg', 'compensationRatio',
]);

export type ParameterSpec = {
  key: ConfigKey; command: string; label: string; group: string; unit?: string;
  hint: string; min: number; max: number; step: number; scale?: number;
  options?: { value: number; label: string }[];
};
const directionOptions = [{ value: 1, label: '正常' }, { value: -1, label: '反向' }];
export const PARAMETERS: ParameterSpec[] = [
  { key: 'lawMode', command: 'law_mode', label: '控制律', group: '鸭翼控制', hint: '默认旧控制律；部分补偿需先完成台架检查。', min: 0, max: 1, step: 1,
    options: [{ value: 0, label: '旧控制律' }, { value: 1, label: '配平与部分补偿' }] },
  { key: 'trimDeg', command: 'trim_deg', label: '机械配平', group: '鸭翼控制', unit: '°', hint: '手动和辅助模式均生效；鸭翼前缘上偏为正。', min: -90, max: 90, step: 0.1 },
  { key: 'aoaReferenceDeg', command: 'aoa_reference_deg', label: '巡航风标参考', group: '鸭翼控制', unit: '°', hint: '相对风标标定零位；不能代替传感器标定。', min: -90, max: 90, step: 0.1 },
  { key: 'compensationRatio', command: 'compensation_ratio', label: '迎角补偿比例', group: '鸭翼控制', hint: '0 关闭补偿，1 为同角度补偿。', min: 0, max: 1, step: 0.01 },
  { key: 'gain', command: 'gain', label: '旧控制律增益', group: '旧控制律', hint: '仅 law_mode 0 使用。', min: 0, max: 5, step: 0.01 },
  { key: 'authorityDeg', command: 'authority', label: '遥控满杆权限', group: '遥控与舵机', unit: '°', hint: '满杆对应的逻辑舵角。', min: 0, max: 90, step: 0.1 },
  { key: 'limitDeg', command: 'limit', label: '逻辑舵角限幅', group: '遥控与舵机', unit: '°', hint: '不得超过机械安全行程。', min: 0.01, max: 90, step: 0.1 },
  { key: 'servoDirection', command: 'servo_dir', label: '舵机方向', group: '遥控与舵机', hint: '反转整个舵机输出。', min: -1, max: 1, step: 1, options: directionOptions },
  { key: 'rcDirection', command: 'rc_dir', label: '遥控方向', group: '遥控与舵机', hint: '只反转飞手输入。', min: -1, max: 1, step: 1, options: directionOptions },
  { key: 'aoaDirection', command: 'aoa_dir', label: '风标补偿方向', group: '遥控与舵机', hint: '只反转自动补偿。', min: -1, max: 1, step: 1, options: directionOptions },
  { key: 'servoMinUs', command: 'servo_min', label: '舵机最小脉宽', group: '遥控与舵机', unit: 'μs', hint: '500～2500，且为 4 的倍数。', min: 500, max: 2500, step: 4 },
  { key: 'servoMidUs', command: 'servo_mid', label: '舵机中位脉宽', group: '遥控与舵机', unit: 'μs', hint: '须在最小和最大脉宽之间。', min: 500, max: 2500, step: 1 },
  { key: 'servoMaxUs', command: 'servo_max', label: '舵机最大脉宽', group: '遥控与舵机', unit: 'μs', hint: '500～2500，且为 4 的倍数。', min: 500, max: 2500, step: 4 },
  { key: 'filterTauUs', command: 'filter_ms', label: '风标滤波时间', group: '响应', unit: 'ms', hint: '数值越大越平滑，也会增加延迟。', min: 1, max: 1000, step: 0.1, scale: 1000 },
  { key: 'slewDegSec', command: 'slew', label: '舵令变化速率', group: '响应', unit: '°/s', hint: '最终舵令的最大变化速度。', min: 10, max: 1000, step: 1 },
  { key: 'throttleSchedule', command: 'throttle_schedule', label: '旧控制律调度', group: '旧控制律', hint: '部分补偿控制律只能使用固定模式。', min: 0, max: 2, step: 1,
    options: [{ value: 0, label: '固定' }, { value: 1, label: '油门代理' }, { value: 2, label: '速度模型' }] },
  { key: 'throttleIdleScale', command: 'throttle_idle', label: '低油门系数', group: '旧控制律', hint: '不能小于高油门系数。', min: 0, max: 1, step: 0.01 },
  { key: 'throttleFullScale', command: 'throttle_full', label: '高油门系数', group: '旧控制律', hint: '不能高于低油门系数。', min: 0, max: 1, step: 0.01 },
  { key: 'throttleTauUs', command: 'throttle_tau_ms', label: '油门滤波时间', group: '旧控制律', unit: 'ms', hint: '仅油门代理与速度模型使用。', min: 10, max: 5000, step: 1, scale: 1000 },
  { key: 'modelMassGrams', command: 'model_mass_g', label: '起飞重量', group: '机型模型', unit: 'g', hint: '0 表示未填写。', min: 0, max: 20000, step: 1 },
  { key: 'modelStaticThrustGrams', command: 'model_thrust_g', label: '静推力', group: '机型模型', unit: 'g', hint: '0 表示未填写。', min: 0, max: 50000, step: 1 },
  { key: 'modelWingAreaDm2', command: 'model_area_dm2', label: '参考翼面积', group: '机型模型', unit: 'dm²', hint: '0 表示未填写。', min: 0, max: 500, step: 0.1 },
  { key: 'modelInitialSpeedMps', command: 'model_initial_mps', label: '初始速度假设', group: '机型模型', unit: 'm/s', hint: '模型首次启动时使用。', min: 0, max: 100, step: 0.1 },
  { key: 'modelDragPreset', command: 'model_drag', label: '阻力档位', group: '机型模型', hint: '速度仅是无独立观测的估算。', min: 1, max: 3, step: 1,
    options: [{ value: 1, label: '低' }, { value: 2, label: '中' }, { value: 3, label: '高' }] },
];

export function normalizeConfig(config: ControlConfig): ControlConfig {
  const copy = { ...config };
  for (const key of CONFIG_KEYS) if (FLOAT_KEYS.has(key)) copy[key] = Math.fround(copy[key]);
  return copy;
}

export type ConfigIssue = { keys: ConfigKey[]; message: string };

export function parameterSupported(key: ConfigKey, version: number): boolean {
  if (version < 6 && ['trimDeg', 'aoaReferenceDeg', 'compensationRatio', 'lawMode'].includes(key)) return false;
  if (version < 5 && key.startsWith('model')) return false;
  return true;
}

export function configIssues(c: ControlConfig): ConfigIssue[] {
  const issues: ConfigIssue[] = [];
  for (const key of CONFIG_KEYS) if (!Number.isFinite(c[key])) issues.push({ keys: [key], message: `${key} 不是有限数值` });
  if (issues.length) return issues;
  const range = (ok: boolean, keys: ConfigKey[], message: string) => { if (!ok) issues.push({ keys, message }); };
  range(c.gain >= 0 && c.gain <= 5, ['gain'], '旧控制律增益须在 0～5');
  range(c.authorityDeg >= 0 && c.authorityDeg <= 90, ['authorityDeg'], '遥控权限须在 0～90°');
  range(c.limitDeg > 0 && c.limitDeg <= 90, ['limitDeg'], '舵角限幅须在 (0, 90]°');
  range(c.filterTauUs >= 1000 && c.filterTauUs <= 1_000_000, ['filterTauUs'], '风标滤波须在 1～1000 ms');
  range(c.slewDegSec >= 10 && c.slewDegSec <= 1000, ['slewDegSec'], '舵令速率须在 10～1000°/s');
  range(c.throttleIdleScale >= 0 && c.throttleIdleScale <= 1 &&
    c.throttleFullScale >= 0 && c.throttleFullScale <= c.throttleIdleScale,
  ['throttleIdleScale', 'throttleFullScale'], '油门系数范围或顺序错误');
  range(c.throttleTauUs >= 10_000 && c.throttleTauUs <= 5_000_000, ['throttleTauUs'], '油门滤波须在 10～5000 ms');
  range(Number.isInteger(c.servoMinUs) && Number.isInteger(c.servoMidUs) && Number.isInteger(c.servoMaxUs) &&
    c.servoMinUs >= 500 && c.servoMaxUs <= 2500 && c.servoMinUs < c.servoMidUs &&
    c.servoMidUs < c.servoMaxUs && c.servoMinUs % 4 === 0 && c.servoMaxUs % 4 === 0,
  ['servoMinUs', 'servoMidUs', 'servoMaxUs'], '舵机端点须有序，最小/最大值为 4 μs 的倍数');
  range([c.servoDirection, c.rcDirection, c.aoaDirection].every(v => v === 1 || v === -1),
    ['servoDirection', 'rcDirection', 'aoaDirection'], '方向只能为 ±1');
  range(Number.isInteger(c.throttleSchedule) && c.throttleSchedule >= 0 && c.throttleSchedule <= 2,
    ['throttleSchedule'], '调度模式无效');
  range((c.modelMassGrams === 0 || c.modelMassGrams >= 10) && c.modelMassGrams <= 20_000 &&
    c.modelStaticThrustGrams >= 0 && c.modelStaticThrustGrams <= 50_000 &&
    (c.modelWingAreaDm2 === 0 || c.modelWingAreaDm2 >= 0.1) && c.modelWingAreaDm2 <= 500 &&
    c.modelInitialSpeedMps >= 0 && c.modelInitialSpeedMps <= 100 &&
    Number.isInteger(c.modelDragPreset) && c.modelDragPreset >= 1 && c.modelDragPreset <= 3,
  ['modelMassGrams', 'modelStaticThrustGrams', 'modelWingAreaDm2', 'modelInitialSpeedMps', 'modelDragPreset'], '机型模型参数范围错误');
  range(c.throttleSchedule !== 2 || (c.modelMassGrams > 0 && c.modelStaticThrustGrams > 0 && c.modelWingAreaDm2 > 0),
    ['throttleSchedule', 'modelMassGrams', 'modelStaticThrustGrams', 'modelWingAreaDm2'], '速度模型调度需要完整机型参数');
  range(Math.abs(c.trimDeg) <= c.limitDeg, ['trimDeg', 'limitDeg'], '配平角不能超过舵角限幅');
  range(Math.abs(c.aoaReferenceDeg) <= 90, ['aoaReferenceDeg'], '风标参考角须在 ±90°');
  range(c.compensationRatio >= 0 && c.compensationRatio <= 1, ['compensationRatio'], '补偿比例须在 0～1');
  range(Number.isInteger(c.lawMode) && (c.lawMode === 0 || c.lawMode === 1), ['lawMode'], '控制律模式无效');
  range(c.lawMode !== 1 || c.throttleSchedule === 0, ['lawMode', 'throttleSchedule'], '部分补偿控制律要求固定调度模式');
  return issues;
}

export function configErrors(c: ControlConfig): string[] { return configIssues(c).map(issue => issue.message); }

function bytesFromHex(hex: string): Uint8Array {
  if (hex.length % 2 || !/^[0-9a-f]+$/i.test(hex)) throw new Error('配置十六进制记录无效');
  return Uint8Array.from(hex.match(/../g) || [], pair => parseInt(pair, 16));
}
function hexFromBytes(bytes: Uint8Array): string {
  return Array.from(bytes, value => value.toString(16).padStart(2, '0')).join('');
}

export function decodeConfigLine(line: string): ConfigSnapshot {
  const fields = line.trim().split(',');
  if (fields.length !== 4 || fields[0] !== 'C' || !['4', '5', '6'].includes(fields[1])) throw new Error('配置记录版本不受支持');
  const version = Number(fields[1]) as 4 | 5 | 6;
  const data = bytesFromHex(fields[2]);
  if (data.length !== (version === 4 ? 42 : version === 5 ? 59 : 72)) throw new Error('配置记录长度错误');
  const zeroBytes = bytesFromHex(fields[3]);
  if (zeroBytes.length !== 4) throw new Error('风标零位记录长度错误');
  const zeroDeg = new DataView(zeroBytes.buffer).getFloat32(0, true);
  if (!Number.isFinite(zeroDeg) || zeroDeg < -180 || zeroDeg >= 180) throw new Error('风标零位记录无效');
  const view = new DataView(data.buffer);
  let i = 0;
  const f = () => { const v = view.getFloat32(i, true); i += 4; return v; };
  const u16 = () => { const v = view.getUint16(i, true); i += 2; return v; };
  const i8 = () => view.getInt8(i++);
  const u8 = () => view.getUint8(i++);
  const config: ControlConfig = {
    gain: f(), authorityDeg: f(), limitDeg: f(), filterTauUs: f(), slewDegSec: f(),
    throttleIdleScale: f(), throttleFullScale: f(), throttleTauUs: f(),
    servoMinUs: u16(), servoMidUs: u16(), servoMaxUs: u16(),
    servoDirection: i8(), rcDirection: i8(), aoaDirection: i8(), throttleSchedule: u8(),
    modelMassGrams: 0, modelStaticThrustGrams: 0, modelWingAreaDm2: 0,
    modelInitialSpeedMps: 0, modelDragPreset: 2,
    trimDeg: 0, aoaReferenceDeg: 0, compensationRatio: 0, lawMode: 0,
  };
  if (version >= 5) {
    config.modelMassGrams = f(); config.modelStaticThrustGrams = f();
    config.modelWingAreaDm2 = f(); config.modelInitialSpeedMps = f(); config.modelDragPreset = u8();
  }
  if (version === 6) {
    config.trimDeg = f(); config.aoaReferenceDeg = f(); config.compensationRatio = f(); config.lawMode = u8();
  }
  if (configErrors(config).length) throw new Error(`设备配置无效：${configErrors(config).join('；')}`);
  return { version, zeroDeg, config };
}

export function encodeConfigLine(snapshot: ConfigSnapshot): string {
  const { config: c, version, zeroDeg } = snapshot;
  const data = new Uint8Array(version === 4 ? 42 : version === 5 ? 59 : 72);
  const view = new DataView(data.buffer);
  let i = 0;
  const f = (v: number) => { view.setFloat32(i, v, true); i += 4; };
  const u16 = (v: number) => { view.setUint16(i, v, true); i += 2; };
  const i8 = (v: number) => view.setInt8(i++, v);
  const u8 = (v: number) => view.setUint8(i++, v);
  [c.gain, c.authorityDeg, c.limitDeg, c.filterTauUs, c.slewDegSec,
    c.throttleIdleScale, c.throttleFullScale, c.throttleTauUs].forEach(f);
  [c.servoMinUs, c.servoMidUs, c.servoMaxUs].forEach(u16);
  [c.servoDirection, c.rcDirection, c.aoaDirection].forEach(i8); u8(c.throttleSchedule);
  if (version >= 5) {
    [c.modelMassGrams, c.modelStaticThrustGrams, c.modelWingAreaDm2, c.modelInitialSpeedMps].forEach(f);
    u8(c.modelDragPreset);
  }
  if (version === 6) { [c.trimDeg, c.aoaReferenceDeg, c.compensationRatio].forEach(f); u8(c.lawMode); }
  const zero = new Uint8Array(4);
  new DataView(zero.buffer).setFloat32(0, zeroDeg, true);
  return `C,${version},${hexFromBytes(data)},${hexFromBytes(zero)}`;
}

export function changedKeys(current: ControlConfig, desired: ControlConfig): ConfigKey[] {
  return CONFIG_KEYS.filter(key => FLOAT_KEYS.has(key) ?
    Math.fround(current[key]) !== Math.fround(desired[key]) : current[key] !== desired[key]);
}

export function planChanges(current: ControlConfig, desiredInput: ControlConfig, version: number): string[] {
  const desired = normalizeConfig(desiredInput);
  const errors = configErrors(desired);
  if (errors.length) throw new Error(errors.join('；'));
  const pending = changedKeys(current, desired);
  if (version < 6 && pending.some(key => ['trimDeg', 'aoaReferenceDeg', 'compensationRatio', 'lawMode'].includes(key)))
    throw new Error('设备固件版本不支持配平与部分补偿参数');
  if (version < 5 && pending.some(key => key.startsWith('model')))
    throw new Error('设备固件版本不支持机型模型参数');
  const state = { ...current };
  const commands: string[] = [];
  while (pending.length) {
    const index = pending.findIndex(key => {
      const next = { ...state, [key]: desired[key] };
      return configErrors(next).length === 0;
    });
    if (index < 0) throw new Error('这些参数无法逐项安全应用；请先用设备恢复默认值或分批调整');
    const [key] = pending.splice(index, 1);
    state[key] = desired[key];
    const spec = PARAMETERS.find(item => item.key === key)!;
    const number = desired[key] / (spec.scale || 1);
    commands.push(`set ${spec.command} ${Number(number.toPrecision(9))}`);
  }
  return commands;
}
