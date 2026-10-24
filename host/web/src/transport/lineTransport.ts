export interface LineTransport {
  readonly kind: 'serial' | 'simulation';
  connect(onLine: (line: string) => void, onDisconnect: (reason?: string) => void): Promise<void>;
  send(line: string): Promise<void>;
  close(): Promise<void>;
}

type SerialPortLike = {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
};
type SerialAccess = { requestPort(): Promise<SerialPortLike> };

export function hasWebSerial(): boolean {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

export class WebSerialTransport implements LineTransport {
  readonly kind = 'serial';
  private port?: SerialPortLike;
  private reader?: ReadableStreamDefaultReader<Uint8Array>;
  private readTask?: Promise<void>;
  private closing = false;
  private onLine: (line: string) => void = () => {};
  private onDisconnect: (reason?: string) => void = () => {};

  async connect(onLine: (line: string) => void, onDisconnect: (reason?: string) => void): Promise<void> {
    if (!hasWebSerial()) throw new Error('此浏览器没有可用的串口接口。请在支持 Web Serial 的安全页面打开。');
    this.onLine = onLine;
    this.onDisconnect = onDisconnect;
    this.closing = false;
    const serial = (navigator as Navigator & { serial: SerialAccess }).serial;
    this.port = await serial.requestPort();
    await this.port.open({ baudRate: 500000 });
    this.readTask = this.readLoop();
  }

  private async readLoop(): Promise<void> {
    const decoder = new TextDecoder();
    let buffer = '';
    let failure: string | undefined;
    try {
      while (this.port?.readable && !this.closing) {
        this.reader = this.port.readable.getReader();
        try {
          while (!this.closing) {
            const { value, done } = await this.reader.read();
            if (done) { failure = '串口读取结束'; break; }
            buffer += decoder.decode(value, { stream: true });
            let newline: number;
            while ((newline = buffer.indexOf('\n')) >= 0) {
              const line = buffer.slice(0, newline).replace(/\r$/, '');
              buffer = buffer.slice(newline + 1);
              if (line) this.onLine(line);
            }
            if (buffer.length > 8192) throw new Error('串口单行超过长度限制');
          }
        } catch (error) {
          if (!this.closing) failure = String(error);
        } finally {
          this.reader.releaseLock();
          this.reader = undefined;
        }
        if (failure) break;
      }
    } finally {
      if (!this.closing) this.onDisconnect(failure || '串口已断开');
    }
  }

  async send(line: string): Promise<void> {
    if (!this.port?.writable) throw new Error('串口未连接');
    const writer = this.port.writable.getWriter();
    try { await writer.write(new TextEncoder().encode(`${line}\n`)); }
    finally { writer.releaseLock(); }
  }

  async close(): Promise<void> {
    this.closing = true;
    try { await this.reader?.cancel(); } catch { /* Port may already be gone. */ }
    await this.readTask;
    try { await this.port?.close(); } catch { /* Physical disconnect already closed it. */ }
    this.port = undefined;
  }
}
