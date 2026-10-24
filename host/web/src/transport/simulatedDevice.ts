import { configErrors, DEFAULT_CONFIG, encodeConfigLine, normalizeConfig, PARAMETERS, type ControlConfig, type ConfigKey } from '../protocol/config';
import type { LineTransport } from './lineTransport';

export class SimulatedDevice implements LineTransport {
  readonly kind = 'simulation';
  private config: ControlConfig = normalizeConfig(DEFAULT_CONFIG);
  private saved: ControlConfig = { ...this.config };
  private onLine: (line: string) => void = () => {};
  private onDisconnect: (reason?: string) => void = () => {};
  private timer?: ReturnType<typeof setInterval>;
  private logging = true;
  private sequence = 0;
  private timeUs = 0;
  private ground = true;

  async connect(onLine: (line: string) => void, onDisconnect: (reason?: string) => void): Promise<void> {
    this.onLine = onLine;
    this.onDisconnect = onDisconnect;
    this.timer = setInterval(() => this.sample(), 50);
  }

  private emit(line: string): void { queueMicrotask(() => this.onLine(line)); }
  private event(name: string): void { this.emit(`E,${name},0`); }
  private metadata(): void { this.emit(encodeConfigLine({ version: 6, config: this.config, zeroDeg: 0 })); }

  async send(line: string): Promise<void> {
    if (line === 'status') { this.event(this.ground ? 'GROUND' : 'RUN_LOCKED'); this.metadata(); return; }
    if (line === 'log stop') { this.logging = false; this.event('LOG_OFF'); return; }
    if (line === 'log start') { this.logging = true; this.event('LOG_ON'); this.metadata(); return; }
    if (!this.ground) { this.event('ERR_LOCKED'); return; }
    if (line === 'run') { this.ground = false; this.event('RUN_LOCKED'); return; }
    if (line === 'save') { this.saved = { ...this.config }; this.event('SAVED'); return; }
    if (line === 'defaults') { this.config = normalizeConfig(DEFAULT_CONFIG); this.event('DEFAULTS_UNSAVED'); this.metadata(); return; }
    const match = /^set ([a-z_]+) (-?(?:\d+\.?\d*|\.\d+))$/.exec(line);
    if (!match) { this.event('ERR_COMMAND'); return; }
    const spec = PARAMETERS.find(item => item.command === match[1]);
    if (!spec) { this.event('ERR_PARAMETER'); return; }
    const next = { ...this.config, [spec.key]: Number(match[2]) * (spec.scale || 1) } as ControlConfig;
    const normalized = normalizeConfig(next);
    if (configErrors(normalized).length) { this.event('ERR_PARAMETER'); return; }
    this.config = normalized;
    this.event('SET_UNSAVED');
    this.metadata();
  }

  private sample(): void {
    if (!this.logging) return;
    this.sequence += 1;
    this.timeUs += 50_000;
    const aoa = Math.sin(this.sequence / 22) * 600;
    const cmd = this.ground ? 0 : this.config.trimDeg * 100 -
      (this.config.lawMode ? this.config.compensationRatio * aoa : this.config.gain * aoa);
    const gain = this.ground ? 0 : this.config.lawMode ? this.config.compensationRatio : this.config.gain;
    const source = this.ground ? 0 : this.config.lawMode ? 4 : this.config.throttleSchedule ? 2 : 1;
    this.emit(`D,${this.sequence},${this.timeUs},5000,512,1500,${this.ground ? 1000 : 2000},1000,${this.ground ? 31 : 15},${this.ground ? 7 : 4},${Math.trunc(cmd)},1500,${Math.trunc(gain * 1000)},${this.ground ? 0 : 1000},${source},0,0,0,5000,0,0,0`);
  }

  async close(): Promise<void> {
    if (this.timer) clearInterval(this.timer);
    this.timer = undefined;
    this.onDisconnect();
  }

  // Test helper: simulate reboot and verify that only saved values persist.
  reboot(): void {
    this.config = { ...this.saved };
    this.sequence = 0;
    this.timeUs = 0;
  }
  setGround(ground: boolean): void {
    this.ground = ground;
    this.event(ground ? 'GROUND' : 'RUN_LOCKED');
  }
  getConfigValue(key: ConfigKey): number { return this.config[key]; }
}
