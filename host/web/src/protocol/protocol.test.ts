import { describe, expect, it } from 'vitest';
import { configErrors, decodeConfigLine, DEFAULT_CONFIG, encodeConfigLine, planChanges } from './config';
import { DeviceSession } from './deviceSession';
import { decodeDataLine, parseLog } from './log';
import { profileFromJson } from './profile';
import { SimulatedDevice } from '../transport/simulatedDevice';
import type { LineTransport } from '../transport/lineTransport';

const firmwareV6 = '9a99993f0000b4420000f041aa5add460000b4430000803f0000003f0024f448bc027805fc0801ff01000000484400007a440000a0410000a040020000a040000040400000003f01';
const zero = '00000000';

describe('firmware records', () => {
  it('reads independent v4/v5/v6 binary layouts', () => {
    const v6 = decodeConfigLine(`C,6,${firmwareV6},${zero}`);
    expect(v6.config.servoMinUs).toBe(700);
    expect(v6.config.rcDirection).toBe(-1);
    expect(v6.config.trimDeg).toBe(5);
    expect(v6.config.aoaReferenceDeg).toBe(3);
    expect(v6.config.compensationRatio).toBe(0.5);
    expect(v6.config.lawMode).toBe(1);
    const v5 = decodeConfigLine(`C,5,${firmwareV6.slice(0, 118)},${zero}`);
    expect(v5.config.modelMassGrams).toBe(800);
    expect(v5.config.lawMode).toBe(0);
    const v4 = decodeConfigLine(`C,4,${firmwareV6.slice(0, 84)},${zero}`);
    expect(v4.config.modelMassGrams).toBe(0);
    expect(v4.config.trimDeg).toBe(0);
    expect(() => decodeConfigLine(`C,6,${firmwareV6.slice(0, 118)},${zero}`)).toThrow();
    expect(encodeConfigLine(v6)).toBe(`C,6,${firmwareV6},${zero}`);
  });

  it('orders coupled fields so every firmware set remains valid', () => {
    const current = { ...DEFAULT_CONFIG, modelMassGrams: 800, modelStaticThrustGrams: 1000,
      modelWingAreaDm2: 20, throttleSchedule: 2 };
    const newLaw = { ...current, throttleSchedule: 0, lawMode: 1, trimDeg: 5,
      aoaReferenceDeg: 3, compensationRatio: 0.5 };
    const forward = planChanges(current, newLaw, 6);
    expect(forward.indexOf('set throttle_schedule 0')).toBeLessThan(forward.indexOf('set law_mode 1'));
    const reverse = planChanges(newLaw, current, 6);
    expect(reverse.indexOf('set law_mode 0')).toBeLessThan(reverse.indexOf('set throttle_schedule 2'));
    const wideTrim = { ...newLaw, trimDeg: 50 };
    const narrower = { ...wideTrim, limitDeg: 30, trimDeg: 20 };
    expect(planChanges(wideTrim, narrower, 6)).toEqual(['set trim_deg 20', 'set limit 30']);
    expect(configErrors({ ...newLaw, throttleSchedule: 2 })).not.toEqual([]);
  });

  it('decodes control data and rejects missing configuration', () => {
    const line = 'D,4,20000,5000,512,1500,2000,1000,15,4,-125,1480,500,1000,4,0,0,0,5000,0,0,0';
    expect(decodeDataLine(line, 6).commandDeg).toBe(-1.25);
    const parsed = parseLog(`#EasyCanadLog\nC,6,${firmwareV6},${zero}\nR,abc\n${line}\n`);
    expect(parsed.frames).toHaveLength(1);
    expect(() => parseLog(line)).toThrow('缺少配置');
  });
});

describe('ground configuration session', () => {
  it('applies, saves and reads back after a simulated reboot', async () => {
    const transport = new SimulatedDevice();
    const session = new DeviceSession(transport);
    const original = await session.connect();
    expect(session.state.ground).toBe(true);
    const desired = { ...original.config, lawMode: 1, trimDeg: 5,
      aoaReferenceDeg: 3, compensationRatio: 0.5 };
    const applied = await session.apply(desired);
    expect(applied.config.trimDeg).toBe(5);
    expect(session.state.saved).toBe(false);
    await session.save(desired);
    expect(session.state.saved).toBe(true);
    transport.reboot();
    expect((await session.refresh()).config.compensationRatio).toBe(0.5);
    transport.setGround(false);
    await Promise.resolve();
    await expect(session.apply({ ...desired, trimDeg: 4 })).rejects.toThrow('地面配置');
    await session.close();
  });

  it('keeps partial writes visible after a failed command', async () => {
    class RejectSecondSet implements LineTransport {
      readonly kind = 'simulation';
      private inner = new SimulatedDevice();
      private onLine: (line: string) => void = () => {};
      private sets = 0;
      connect(onLine: (line: string) => void, onDisconnect: (reason?: string) => void): Promise<void> {
        this.onLine = onLine;
        return this.inner.connect(onLine, onDisconnect);
      }
      async send(line: string): Promise<void> {
        if (line.startsWith('set ') && ++this.sets === 2) {
          queueMicrotask(() => this.onLine('E,ERR_PARAMETER,0'));
        } else await this.inner.send(line);
      }
      close(): Promise<void> { return this.inner.close(); }
    }
    const session = new DeviceSession(new RejectSecondSet());
    const original = await session.connect();
    await expect(session.apply({ ...original.config, gain: 0.9, trimDeg: 5 })).rejects.toThrow('ERR_PARAMETER');
    expect(session.state.saved).toBe(false);
    expect(session.state.config?.config.gain).toBeCloseTo(0.9);
    expect(session.state.config?.config.trimDeg).toBe(0);
    await session.close();
  });

  it('restores defaults without saving and locks writes after RUN', async () => {
    const session = new DeviceSession(new SimulatedDevice());
    const original = await session.connect();
    await session.save({ ...original.config, trimDeg: 4 });
    expect(session.state.saved).toBe(true);
    expect((await session.defaults()).config.trimDeg).toBe(0);
    expect(session.state.saved).toBe(false);
    await session.run();
    expect(session.state.ground).toBe(false);
    await expect(session.defaults()).rejects.toThrow('地面配置');
    await session.close();
  });
});

describe('model profiles', () => {
  it('migrates the existing v1 file fields without changing unrelated control values', () => {
    const old = { format: 'easycanad-model', version: 1, name: '样机', mass_g: 800,
      static_thrust_g: 1000, wing_area_dm2: 20, drag: 'medium', initial_speed_mps: 0,
      use_model_gain: true };
    const result = profileFromJson(old, { ...DEFAULT_CONFIG, trimDeg: 3 });
    expect(result.config.modelMassGrams).toBe(800);
    expect(result.config.throttleSchedule).toBe(2);
    expect(result.config.trimDeg).toBe(3);
    expect(profileFromJson(result).config).toEqual(result.config);
    expect(() => profileFromJson({ ...old, use_model_gain: 'false' })).toThrow();
    expect(() => profileFromJson({ ...result, config: { ...result.config, gain: '1.2' } })).toThrow();
  });
});
