import { configErrors, CONFIG_KEYS, DEFAULT_CONFIG, normalizeConfig, type ControlConfig } from './config';

export type Profile = {
  format: 'easycanad-profile'; version: 2; name: string; notes: string; config: ControlConfig;
};

function object(value: unknown): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error('机型文件格式错误');
  return value as Record<string, unknown>;
}

function label(value: unknown, field: string): string {
  if (typeof value !== 'string' || !value.trim() || value.length > 80) throw new Error(`${field}须为 1～80 个字符`);
  return value.trim();
}

function number(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error(`${field}须为有限数值`);
  return value;
}

export function profileFromJson(value: unknown, current: ControlConfig = DEFAULT_CONFIG): Profile {
  const data = object(value);
  if (data.format === 'easycanad-model' && data.version === 1) {
    const drag = { low: 1, medium: 2, high: 3 }[data.drag as 'low' | 'medium' | 'high'];
    if (!drag || typeof data.use_model_gain !== 'boolean') throw new Error('旧版机型档位或调度开关无效');
    const config: ControlConfig = {
      ...current,
      modelMassGrams: number(data.mass_g, '起飞重量'),
      modelStaticThrustGrams: number(data.static_thrust_g, '静推力'),
      modelWingAreaDm2: number(data.wing_area_dm2, '翼面积'),
      modelInitialSpeedMps: number(data.initial_speed_mps, '初始速度'),
      modelDragPreset: drag,
      throttleSchedule: data.use_model_gain ? 2 : 0,
      lawMode: data.use_model_gain ? 0 : current.lawMode,
    };
    if (configErrors(config).length) throw new Error('旧版机型参数无效或与当前配置不兼容');
    return { format: 'easycanad-profile', version: 2, name: label(data.name, '机型名称'),
      notes: '从旧版机型文件导入', config: normalizeConfig(config) };
  }
  if (data.format !== 'easycanad-profile' || data.version !== 2) throw new Error('机型文件版本不受支持');
  const source = object(data.config);
  const config = {} as ControlConfig;
  for (const key of CONFIG_KEYS) config[key] = number(source[key], key);
  if (configErrors(config).length) throw new Error('机型文件包含无效控制参数');
  if (data.notes !== undefined && (typeof data.notes !== 'string' || data.notes.length > 500))
    throw new Error('机型备注过长或格式错误');
  return { format: 'easycanad-profile', version: 2, name: label(data.name, '机型名称'),
    notes: (data.notes as string | undefined) ?? '', config: normalizeConfig(config) };
}
