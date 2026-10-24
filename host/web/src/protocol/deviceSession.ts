import { changedKeys, decodeConfigLine, normalizeConfig, planChanges, type ConfigSnapshot, type ControlConfig } from './config';
import { decodeDataLine, type DataFrame } from './log';
import type { LineTransport } from '../transport/lineTransport';

type Waiter = { match: (line: string) => boolean; resolve: (line: string) => void; reject: (error: Error) => void; timer: ReturnType<typeof setTimeout> };
export type DeviceState = {
  connected: boolean; kind: 'serial' | 'simulation'; ground: boolean | null;
  config?: ConfigSnapshot; live?: DataFrame; lastFrameAt?: number; lastEvent?: string;
  saved: boolean | null; recording: boolean; lines: string[]; recordedFrames: number; error?: string;
};

export class DeviceSession {
  readonly state: DeviceState;
  private waiters: Waiter[] = [];
  private listeners = new Set<() => void>();
  private frameListeners = new Set<(frame: DataFrame) => void>();
  private serial = Promise.resolve();
  private lastLiveNotice = 0;

  constructor(private readonly transport: LineTransport) {
    this.state = { connected: false, kind: transport.kind, ground: null, saved: null, recording: false, lines: [], recordedFrames: 0 };
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  subscribeFrames(listener: (frame: DataFrame) => void): () => void {
    this.frameListeners.add(listener);
    return () => this.frameListeners.delete(listener);
  }
  private notify(): void { for (const listener of this.listeners) listener(); }

  private onLine = (line: string): void => {
    if (this.state.recording) {
      this.state.lines.push(line);
      if (line.startsWith('D,')) this.state.recordedFrames += 1;
    }
    try {
      if (line.startsWith('C,')) this.state.config = decodeConfigLine(line);
      else if (line.startsWith('E,')) {
        const event = line.split(',')[1];
        this.state.lastEvent = event;
        if (event === 'GROUND') this.state.ground = true;
        if (event === 'RUN_LOCKED') this.state.ground = false;
        if (event === 'SET_UNSAVED' || event === 'DEFAULTS_UNSAVED') this.state.saved = false;
        if (event === 'SAVED') this.state.saved = true;
      } else if (line.startsWith('D,') && this.state.config) {
        this.state.live = decodeDataLine(line, this.state.config.version);
        this.state.lastFrameAt = performance.now();
        for (const listener of this.frameListeners) listener(this.state.live);
      }
    } catch (error) {
      this.state.error = String(error);
    }
    for (const waiter of [...this.waiters]) if (waiter.match(line)) {
      clearTimeout(waiter.timer);
      this.waiters.splice(this.waiters.indexOf(waiter), 1);
      waiter.resolve(line);
    }
    if (!line.startsWith('D,') || performance.now() - this.lastLiveNotice >= 70) {
      this.lastLiveNotice = performance.now();
      this.notify();
    }
  };

  private disconnected = (reason?: string): void => {
    this.state.connected = false;
    this.state.ground = null;
    this.state.recording = false;
    this.state.live = undefined;
    this.state.lastFrameAt = undefined;
    if (reason) this.state.error = reason;
    for (const waiter of this.waiters.splice(0)) {
      clearTimeout(waiter.timer);
      waiter.reject(new Error(reason || '设备已断开'));
    }
    this.notify();
  };

  private expect(match: (line: string) => boolean, timeoutMs = 5000): { promise: Promise<string>; cancel: () => void } {
    let cancel = () => {};
    const promise = new Promise<string>((resolve, reject) => {
      const waiter: Waiter = {
        match, resolve, reject,
        timer: setTimeout(() => {
          this.waiters.splice(this.waiters.indexOf(waiter), 1);
          reject(new Error('等待设备回复超时'));
        }, timeoutMs),
      };
      this.waiters.push(waiter);
      cancel = () => {
        const index = this.waiters.indexOf(waiter);
        if (index >= 0) this.waiters.splice(index, 1);
        clearTimeout(waiter.timer);
        reject(new Error('命令已取消'));
      };
    });
    void promise.catch(() => {});
    return { promise, cancel };
  }

  private async command(command: string, accepted: string[]): Promise<string> {
    const reply = this.expect(line => line.startsWith('E,'));
    try { await this.transport.send(command); } catch (error) { reply.cancel(); throw error; }
    const line = await reply.promise;
    const event = line.split(',')[1];
    if (!accepted.includes(event)) throw new Error(`设备回复 ${event}，命令 ${command} 未完成`);
    return event;
  }

  private async query(): Promise<ConfigSnapshot> {
    let statusEvent = '';
    const config = this.expect(line => {
      if (line.startsWith('E,')) statusEvent = line.split(',')[1];
      return (statusEvent === 'GROUND' || statusEvent === 'RUN_LOCKED') && line.startsWith('C,');
    });
    try { await this.transport.send('status'); } catch (error) { config.cancel(); throw error; }
    const line = await config.promise;
    const snapshot = decodeConfigLine(line);
    this.state.config = snapshot;
    return snapshot;
  }

  private exclusive<T>(work: () => Promise<T>): Promise<T> {
    const result = this.serial.then(work);
    this.serial = result.then(() => undefined, () => undefined);
    return result;
  }

  async connect(): Promise<ConfigSnapshot> {
    await this.transport.connect(this.onLine, this.disconnected);
    this.state.connected = true;
    this.state.error = undefined;
    this.notify();
    if (this.transport.kind === 'serial') await new Promise(resolve => setTimeout(resolve, 2000));
    return this.refresh();
  }
  refresh(): Promise<ConfigSnapshot> { return this.exclusive(() => this.query()); }

  private async applyInternal(desiredInput: ControlConfig): Promise<ConfigSnapshot> {
    if (!this.state.connected || this.state.ground !== true || !this.state.config) throw new Error('仅地面配置状态允许写入');
    const desired = normalizeConfig(desiredInput);
    const commands = planChanges(this.state.config.config, desired, this.state.config.version);
    if (!commands.length) return this.query();
    await this.command('log stop', ['LOG_OFF']);
    let writeError: unknown;
    try {
      for (const item of commands) {
        await this.command(item, ['SET_UNSAVED']);
        this.state.saved = false;
      }
      const snapshot = await this.query();
      if (changedKeys(snapshot.config, desired).length) throw new Error('设备回读与草稿不一致，请重新读取并检查');
      return snapshot;
    } catch (error) {
      writeError = error;
      try { await this.query(); } catch { /* Preserve the original write error. */ }
      throw error;
    } finally {
      try { await this.command('log start', ['LOG_ON']); }
      catch (error) {
        if (writeError) throw new AggregateError([writeError, error], '参数写入失败，日志恢复也失败');
        throw error;
      }
      this.notify();
    }
  }

  apply(desired: ControlConfig): Promise<ConfigSnapshot> {
    return this.exclusive(() => this.applyInternal(desired));
  }
  save(desired: ControlConfig): Promise<ConfigSnapshot> {
    return this.exclusive(async () => {
      await this.applyInternal(desired);
      await this.command('save', ['SAVED']);
      const snapshot = await this.query();
      if (changedKeys(snapshot.config, normalizeConfig(desired)).length) throw new Error('保存后回读与草稿不一致');
      this.state.saved = true;
      this.notify();
      return snapshot;
    });
  }
  defaults(): Promise<ConfigSnapshot> {
    return this.exclusive(async () => {
      if (this.state.ground !== true) throw new Error('仅地面配置状态允许恢复默认值');
      await this.command('defaults', ['DEFAULTS_UNSAVED']);
      this.state.saved = false;
      return this.query();
    });
  }
  run(): Promise<ConfigSnapshot> {
    return this.exclusive(async () => {
      if (this.state.ground !== true) throw new Error('设备不在地面配置状态');
      await this.command('run', ['RUN_LOCKED']);
      this.state.ground = false;
      this.notify();
      return this.query();
    });
  }
  startRecording(): Promise<void> {
    return this.exclusive(async () => {
      this.state.lines = [];
      this.state.recordedFrames = 0;
      this.state.recording = true;
      try {
        await this.command('log start', ['LOG_ON']);
        await this.query();
        this.notify();
      } catch (error) {
        this.state.recording = false;
        this.notify();
        throw error;
      }
    });
  }
  stopRecording(): void { this.state.recording = false; this.notify(); }
  async close(): Promise<void> {
    await this.transport.close();
    this.disconnected();
  }
}
