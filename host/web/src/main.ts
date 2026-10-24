import './style.css';
import './interactions.css';
import { changedKeys, configErrors, configIssues, DEFAULT_CONFIG, normalizeConfig, parameterSupported, PARAMETERS,
  type ConfigIssue, type ConfigKey, type ControlConfig } from './protocol/config';
import { DeviceSession } from './protocol/deviceSession';
import { frameTimelineUs, framesToCsv, parseLog, STATE_NAMES, type DataFrame, type ParsedLog } from './protocol/log';
import { profileFromJson, type Profile } from './protocol/profile';
import { hasWebSerial, WebSerialTransport } from './transport/lineTransport';
import { SimulatedDevice } from './transport/simulatedDevice';

type Page = 'device' | 'config' | 'profiles' | 'logs';
const EVENT_NAMES: Record<string, string> = {
  GROUND: '地面配置', RUN_LOCKED: '运行状态已锁定', SET_UNSAVED: '参数已应用，尚未保存',
  DEFAULTS_UNSAVED: '已恢复默认，尚未保存', SAVED: '参数已保存', LOG_ON: '数据输出中', LOG_OFF: '数据输出暂停',
};
const app = document.querySelector<HTMLDivElement>('#app')!;
let page: Page = 'device';
let session: DeviceSession | undefined;
let simulation: SimulatedDevice | undefined;
let unsubscribe: (() => void)[] = [];
let draft: ControlConfig | undefined;
let imported: ParsedLog | undefined;
let importedTimes: number[] = [];
let currentRecording: ParsedLog | undefined;
let currentRecordingTimes: number[] = [];
let logSource: 'live' | 'imported' = 'live';
let liveFrames: DataFrame[] = [];
let lastRecording: { kind: 'serial' | 'simulation'; lines: string[]; frames: number } | undefined;
let cursor = 0;
let playTimer: ReturnType<typeof setInterval> | undefined;
let playbackSpeed = 1;
let busy = false;
let toastTimer: ReturnType<typeof setTimeout> | undefined;
let invalidInputs = new Map<ConfigKey, string>();
let recordingExported = true;
const deletedProfiles: { profile: Profile; index: number }[] = [];
let writeWarning: string | undefined;

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character]!);
}
function fmt(value: number, digits = 1): string { return Number.isFinite(value) ? value.toFixed(digits) : '—'; }
function download(name: string, content: string, type: string): void {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const link = document.createElement('a');
  link.href = url; link.download = name; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}
function notice(message: string, error = false): void {
  const element = document.querySelector<HTMLDivElement>('#notice')!;
  element.textContent = message;
  element.className = `notice show${error ? ' error' : ''}`;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => element.className = 'notice', 5500);
}

function confirmAction(title: string, message: string, confirmLabel = '继续'): Promise<boolean> {
  const dialog = document.querySelector<HTMLDialogElement>('#confirm-dialog')!;
  if (dialog.open) return Promise.resolve(false);
  document.querySelector<HTMLElement>('#confirm-title')!.textContent = title;
  document.querySelector<HTMLElement>('#confirm-message')!.textContent = message;
  const confirm = document.querySelector<HTMLButtonElement>('#confirm-accept')!;
  const cancel = document.querySelector<HTMLButtonElement>('#confirm-cancel')!;
  confirm.textContent = confirmLabel;
  return new Promise(resolve => {
    let accepted = false;
    const onAccept = () => { accepted = true; dialog.close(); };
    const onCancel = () => dialog.close();
    const onClose = () => {
      confirm.removeEventListener('click', onAccept);
      cancel.removeEventListener('click', onCancel);
      dialog.removeEventListener('close', onClose);
      resolve(accepted);
    };
    confirm.addEventListener('click', onAccept);
    cancel.addEventListener('click', onCancel);
    dialog.addEventListener('close', onClose);
    dialog.showModal();
  });
}

function hasPendingDraft(): boolean {
  if (!draft) return false;
  if (invalidInputs.size) return true;
  const current = session?.state.config?.config;
  return current ? changedKeys(current, normalizeConfig(draft)).length > 0 : true;
}

function confirmDiscardDraft(actionName: string): Promise<boolean> {
  return hasPendingDraft() ? confirmAction('放弃未应用的草稿？', `${actionName}会替换当前草稿。未应用的修改将丢失。`, '放弃草稿') : Promise.resolve(true);
}

function replaceDraft(config: ControlConfig): void {
  draft = { ...config };
  invalidInputs.clear();
  writeWarning = undefined;
}

function activeLog(): ParsedLog | undefined {
  return logSource === 'imported' ? imported : session?.state.recording ? undefined : currentRecording;
}

function activeLogTimes(): number[] { return logSource === 'imported' ? importedTimes : currentRecordingTimes; }

function parseCurrentRecording(lines: string[]): void {
  try {
    currentRecording = parseLog('#EasyCanadLog\n' + lines.join('\n'));
    currentRecordingTimes = frameTimelineUs(currentRecording.frames);
    cursor = currentRecording.frames.length - 1;
  } catch {
    currentRecording = undefined;
    currentRecordingTimes = [];
  }
}

function describeChanges(before: ControlConfig, after: ControlConfig): string {
  const keys = changedKeys(before, normalizeConfig(after));
  if (!keys.length) return '当前参数没有变化。';
  return keys.map(key => {
    const spec = PARAMETERS.find(item => item.key === key)!;
    const display = (value: number) => spec.options?.find(option => option.value === value)?.label ??
      `${Number((value / (spec.scale || 1)).toFixed(3))}${spec.unit || ''}`;
    return `${spec.label}：${display(before[key])} → ${display(after[key])}`;
  }).join('\n');
}
async function action(work: () => Promise<void>): Promise<void> {
  if (busy) return;
  busy = true; updateTop();
  try { await work(); }
  catch (error) { notice(error instanceof Error ? error.message : String(error), true); }
  finally { busy = false; updateTop(); updateDraftState(); }
}

app.innerHTML = `
  <div class="shell">
    <aside class="sidebar">
      <div class="brand"><div class="brand-mark">E</div><div><strong>EasyCanad</strong><small>鸭翼控制台</small></div></div>
      <div class="side-label">工作区</div>
      <nav id="navigation" aria-label="主导航">
        <button data-page="device"><span class="nav-icon">◉</span><span>设备</span></button>
        <button data-page="config"><span class="nav-icon">◧</span><span>调参</span></button>
        <button data-page="profiles"><span class="nav-icon">▤</span><span>机型</span></button>
        <button data-page="logs"><span class="nav-icon">⌁</span><span>日志</span></button>
      </nav>
      <div class="sidebar-foot"><span class="version">HOST · v0.1</span><span>本地数据 · 离线可用</span></div>
    </aside>
    <div class="workspace">
      <header class="topbar"><div class="breadcrumb">EasyCanad <span>/</span> <b id="page-name">设备</b></div>
        <div class="top-actions"><button id="recording-indicator" class="chip recording-chip" hidden>● 录制中 <span id="recording-count"></span></button><span id="connection-chip" class="chip">未连接</span><span id="mode-chip" class="chip subtle">—</span></div>
      </header>
      <main id="page-content"></main>
      <nav class="mobile-nav" aria-label="手机导航">
        <button data-page="device">◉<span>设备</span></button><button data-page="config">◧<span>调参</span></button>
        <button data-page="profiles">▤<span>机型</span></button><button data-page="logs">⌁<span>日志</span></button>
      </nav>
    </div>
    <div id="notice" class="notice" role="status"></div>
    <dialog id="confirm-dialog" class="confirm-dialog" aria-labelledby="confirm-title" aria-describedby="confirm-message">
      <h2 id="confirm-title"></h2><p id="confirm-message"></p>
      <div class="button-row"><button id="confirm-cancel" class="button secondary">取消</button><button id="confirm-accept" class="button primary">继续</button></div>
    </dialog>
  </div>`;
document.querySelectorAll<HTMLButtonElement>('[data-page]').forEach(button => button.addEventListener('click', () => {
  stopPlayback();
  page = button.dataset.page as Page; renderPage();
}));
document.querySelector<HTMLButtonElement>('#recording-indicator')!.addEventListener('click', () => {
  page = 'logs'; renderPage();
});

function stopPlayback(): void {
  if (playTimer) clearInterval(playTimer);
  playTimer = undefined;
}

function updateTop(): void {
  const state = session?.state;
  document.querySelector<HTMLElement>('.workspace')!.classList.toggle('busy', busy);
  const connection = document.querySelector<HTMLElement>('#connection-chip')!;
  const frameAge = state?.lastFrameAt === undefined ? Infinity : performance.now() - state.lastFrameAt;
  const stale = state?.connected && frameAge > 2000;
  connection.textContent = state?.connected ? stale ? '数据等待中' : state.kind === 'simulation' ? '模拟设备' : '已连接' : '未连接';
  connection.className = `chip ${state?.connected && !stale ? 'online' : ''}`;
  const recording = document.querySelector<HTMLButtonElement>('#recording-indicator')!;
  recording.hidden = !state?.recording;
  recording.disabled = busy;
  if (state?.recording) document.querySelector<HTMLElement>('#recording-count')!.textContent = `${state.recordedFrames} 帧`;
  const mode = document.querySelector<HTMLElement>('#mode-chip')!;
  mode.textContent = state?.ground === true ? 'GROUND · 可配置' : state?.ground === false ? 'RUN · 只读' : '—';
  mode.className = `chip subtle ${state?.ground === true ? 'ground' : ''}`;
  document.querySelector<HTMLElement>('#page-name')!.textContent = ({ device: '设备', config: '调参', profiles: '机型', logs: '日志' })[page];
  document.querySelectorAll<HTMLButtonElement>('[data-page]').forEach(button => {
    button.classList.toggle('active', button.dataset.page === page);
    button.disabled = busy;
  });
  updateLive();
}

async function connect(kind: 'serial' | 'simulation'): Promise<void> {
  if (session) await disconnect();
  simulation = kind === 'simulation' ? new SimulatedDevice() : undefined;
  const next = new DeviceSession(simulation ?? new WebSerialTransport());
  session = next;
  let connected = next.state.connected;
  let ground = next.state.ground;
  unsubscribe = [next.subscribe(() => {
    if (connected !== next.state.connected || ground !== next.state.ground) {
      connected = next.state.connected; ground = next.state.ground; renderPage();
    } else { updateTop(); if (page === 'logs') drawChart(); }
  }),
    next.subscribeFrames(frame => {
      liveFrames.push(frame); if (liveFrames.length > 1200) liveFrames.shift();
      if (next.state.recording) recordingExported = false;
    })];
  try {
    const snapshot = await next.connect();
    replaceDraft(snapshot.config);
    liveFrames = [];
    notice(kind === 'simulation' ? '已连接模拟设备。模拟读数仅供界面验证。' : '已连接，配置已从设备读取。');
  } catch (error) {
    await disconnect();
    throw error;
  }
  renderPage();
}
async function disconnect(): Promise<void> {
  const pendingRecording = !!session?.state.lines.length && !recordingExported;
  unsubscribe.forEach(remove => remove()); unsubscribe = [];
  if (session?.state.lines.length) {
    lastRecording = { kind: session.state.kind,
      lines: [...session.state.lines], frames: session.state.recordedFrames };
    parseCurrentRecording(lastRecording.lines);
  }
  if (session) await session.close();
  session = undefined; simulation = undefined; draft = undefined; invalidInputs.clear(); liveFrames = [];
  renderPage();
  if (pendingRecording) notice('连接已断开。本次记录仍可在日志页导出，关闭页面前请先导出。');
}

function renderPage(): void {
  updateTop();
  if (page === 'device') renderDevice();
  else if (page === 'config') renderConfig();
  else if (page === 'profiles') renderProfiles();
  else renderLogs();
  updateTop();
}

function renderDevice(): void {
  const state = session?.state;
  const serial = hasWebSerial();
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">DEVICE / STATUS</div><h1>设备概览</h1><p>连接控制器，查看实时状态与风标、舵机读数。</p></div>
    <div class="hero-card"><div class="hero-copy"><div class="eyebrow light">连接状态</div>
      <h2>${state?.connected ? (state.kind === 'simulation' ? '模拟设备已连接' : '控制器已连接') : '准备连接 EasyCanad'}</h2>
      <p>${state?.connected ? '配置与状态来自当前连接。运行状态下仅允许查看与记录。' : state?.error ? `连接已中断：${escapeHtml(state.error)}` : '先连接 USB 串口，或使用模拟设备体验完整地面配置流程。'}</p>
      <div class="button-row wrap">${state?.connected ? '<button id="disconnect" class="button light">断开连接</button><button id="refresh" class="button ghost-light">重新读取</button>' :
        `<button id="connect-serial" class="button light" ${serial ? '' : 'disabled'}>连接 USB 串口</button><button id="connect-sim" class="button ghost-light">使用模拟设备</button>`}</div>
      ${!serial && !state?.connected ? '<small>当前浏览器未提供 Web Serial；模拟设备仍可使用。USB 串口通常需要安全页面与受支持的浏览器。</small>' : ''}
    </div><div class="hero-visual"><div class="radar"><div class="radar-line one"></div><div class="radar-line two"></div><div class="radar-point"></div><span>AoA</span></div></div></div>
    <div class="stat-grid">
      <div class="stat-card"><span>控制状态</span><strong id="metric-state">—</strong><small id="metric-state-detail">等待设备输入</small></div>
      <div class="stat-card"><span>风标读数</span><strong id="metric-aoa">—</strong><small>相对标定零位</small></div>
      <div class="stat-card"><span>鸭翼舵令</span><strong id="metric-command">—</strong><small id="metric-servo">舵机脉宽 —</small></div>
      <div class="stat-card"><span>辅助比例</span><strong id="metric-gain">—</strong><small id="metric-model">等待实时数据</small></div>
    </div>
    <div class="two-column"><section class="panel"><div class="panel-title"><div><div class="eyebrow">CONTROL</div><h3>输入与模式</h3></div><span id="device-mode-tag" class="pill">—</span></div>
      <div class="detail-grid"><div><span>俯仰输入</span><b id="detail-rc">—</b></div><div><span>模式输入</span><b id="detail-mode">—</b></div>
      <div><span>油门输入</span><b id="detail-throttle">—</b></div><div><span>协议版本</span><b id="detail-version">—</b></div>
      <div><span>数据接收</span><b id="detail-health">—</b></div><div><span>最近事件或错误</span><b id="detail-event">—</b></div></div>
      ${state?.ground === true ? '<div class="panel-actions"><button id="enter-run" class="button secondary">手动进入 RUN</button><p class="subtle-note">先拔掉 D5 跳线，遥控置手动且俯仰回中；设备会自行检查条件。</p></div>' : ''}
      ${simulation ? '<div class="panel-actions"><button id="toggle-sim-mode" class="button secondary">模拟切换 GROUND / RUN</button></div>' : ''}</section>
      <section class="panel"><div class="panel-title"><div><div class="eyebrow">CALIBRATION</div><h3>风标零位</h3></div><span id="zero-value" class="pill">—</span></div>
      <p id="cal-status" class="body-copy">标定状态 —</p>
      <p class="body-copy">仅在地面配置状态、遥控手动且俯仰回中时，长按设备 D4 按钮约 1.5 秒。标定采样需要风标保持稳定。</p>
      <p class="subtle-note">串口连接可能使 Nano 重启。进入 RUN 仍由地面跳线和设备命令条件控制；本界面不会自动进入 RUN。</p></section></div>`;
  document.querySelector<HTMLButtonElement>('#connect-serial')?.addEventListener('click', () => void (async () => {
    if (!session || await confirmDiscardDraft('连接另一台设备')) await action(() => connect('serial'));
  })());
  document.querySelector<HTMLButtonElement>('#connect-sim')?.addEventListener('click', () => void (async () => {
    if (!session || await confirmDiscardDraft('连接另一台设备')) await action(() => connect('simulation'));
  })());
  document.querySelector<HTMLButtonElement>('#disconnect')?.addEventListener('click', () => void (async () => {
    if (await confirmDiscardDraft('断开连接')) await action(disconnect);
  })());
  document.querySelector<HTMLButtonElement>('#refresh')?.addEventListener('click', () => void action(async () => {
    if (!await confirmDiscardDraft('重新读取设备配置')) return;
    const snapshot = await session!.refresh(); replaceDraft(snapshot.config); notice('已从设备重新读取配置。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#enter-run')?.addEventListener('click', () => void action(async () => {
    const pending = hasPendingDraft() ? '当前草稿尚未应用，将无法在 RUN 状态写入。\n' : '';
    const unsaved = session?.state.saved === false ? '当前设备参数尚未持久保存。\n' : '';
    if (!await confirmAction('确认进入 RUN？', `${pending}${unsaved}请确认已拔掉 D5 跳线、遥控处于手动且俯仰回中。进入后，本次上电内不能返回 GROUND 配置。`, '进入 RUN')) return;
    await session!.run(); notice('设备已进入 RUN，参数改为只读。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#toggle-sim-mode')?.addEventListener('click', () => {
    const toGround = session?.state.ground !== true;
    simulation!.setGround(toGround);
    notice(toGround ? '模拟设备处于 GROUND。' : '模拟设备处于 RUN，只读。');
    renderPage();
  });
  updateLive();
}

function updateLive(): void {
  const state = session?.state;
  const age = state?.lastFrameAt === undefined ? Infinity : performance.now() - state.lastFrameAt;
  const frame = state?.connected && age <= 2000 ? state.live : undefined;
  const set = (id: string, value: string) => { const node = document.querySelector<HTMLElement>(`#${id}`); if (node) node.textContent = value; };
  set('metric-state', frame ? STATE_NAMES[frame.state] : '—');
  set('metric-state-detail', frame ? `控制周期 ${fmt(frame.elapsedUs / 1000, 0)} ms` : state?.connected && state.lastFrameAt !== undefined ? '超过 2 秒未收到数据' : '等待设备输入');
  const aoa = frame && state?.config ? relativeAoa(frame, state.config.zeroDeg) : undefined;
  set('metric-aoa', aoa === undefined ? '—' : `${fmt(aoa)}°`);
  set('metric-command', frame ? `${fmt(frame.commandDeg)}°` : '—');
  set('metric-servo', frame ? `舵机脉宽 ${frame.servoUs} μs` : '舵机脉宽 —');
  set('metric-gain', frame ? fmt(frame.effectiveGain, 2) : '—');
  const modelStatus = ['等待', '预测中（未观测校正）', '输入中断'][frame?.modelStatus ?? -1] ?? '—';
  set('metric-model', frame ? `模型估算速度 ${fmt(frame.modelSpeedMps)} m/s · ${modelStatus}` : '等待实时数据');
  set('detail-rc', frame ? `${frame.rcUs} μs · ${frame.flags & 1 ? '有效' : '无效'}` : '—');
  set('detail-mode', frame ? `${frame.modeUs} μs · ${frame.flags & 2 ? '有效' : '无效'}` : '—');
  set('detail-throttle', frame ? `${frame.throttleUs} μs · ${frame.flags & 4 ? '有效' : '无效'}` : '—');
  set('detail-version', state?.config ? `v${state.config.version}` : '—');
  set('detail-health', frame ? `正常 · 丢帧 ${frame.dropped} · 漏周期 ${frame.missed}` : state?.connected ? '等待新数据' : '未连接');
  set('detail-event', state?.error || (state?.lastEvent ? EVENT_NAMES[state.lastEvent] || state.lastEvent : '—'));
  set('device-mode-tag', state?.ground === true ? 'GROUND' : state?.ground === false ? 'RUN' : '—');
  set('zero-value', state?.config ? `${fmt(state.config.zeroDeg, 2)}°` : '—');
  const calStatus = ['空闲', '采样中', '已保存', '拒绝或失败'][frame?.calibrationStatus ?? -1] ?? '—';
  set('cal-status', `标定状态 ${calStatus}${frame && !(frame.flags & 8) ? ' · 零位无效' : ''}`);
  const selected = activeLog()?.frames[cursor];
  set('log-live-state', selected ? STATE_NAMES[selected.state] : frame ? STATE_NAMES[frame.state] : '—');
  const recordedCount = state?.recordedFrames || lastRecording?.frames || 0;
  set('log-live-count', recordedCount ? `${recordedCount} 帧` : '未记录');
}

function relativeAoa(frame: DataFrame, zero: number): number {
  let angle = -180 + frame.aoaAdc * 360 / 1023 - zero;
  while (angle >= 180) angle -= 360;
  while (angle < -180) angle += 360;
  return angle;
}

function renderConfig(): void {
  const state = session?.state;
  const value = draft ?? state?.config?.config;
  const groups = [...new Set(PARAMETERS.map(item => item.group))];
  invalidInputs.clear();
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">PARAMETERS / CONFIGURATION</div><h1>控制参数</h1><p>先修改本地草稿，再应用并从设备回读。保存会写入设备的 EEPROM。</p></div>
    <div class="workflow-bar"><div><span class="workflow-dot"></span><strong id="draft-state">${state?.connected ? '已读取设备' : '未连接设备'}</strong><small id="draft-detail">${state?.ground === true ? '地面配置可写' : '连接 GROUND 设备后可编辑'}</small></div>
      <div class="button-row wrap"><button id="read-config" class="button secondary" ${state?.connected ? '' : 'disabled'}>重新读取</button>
      <button id="apply-config" class="button secondary" disabled>应用到设备</button><button id="save-config" class="button primary" disabled>保存到设备</button>
      ${state?.ground === true ? '<button id="restore-defaults" class="button secondary">恢复默认</button>' : ''}</div></div>
    ${writeWarning ? `<div class="write-warning" role="alert">${escapeHtml(writeWarning)}</div>` : ''}
    <div id="config-errors" class="validation validation-summary" aria-live="polite"></div>
    ${!value ? '<div class="empty-state"><div class="empty-symbol">◧</div><h3>尚无设备配置</h3><p>连接控制器或模拟设备后，即可查看全部参数。</p></div>' :
      `<div class="config-layout"><div class="config-main">${groups.map(group => `<section class="panel config-group"><div class="panel-title"><div><div class="eyebrow">${escapeHtml(group === '鸭翼控制' ? 'CONTROL LAW' : group === '机型模型' ? 'AIRFRAME' : 'SETTINGS')}</div><h3>${escapeHtml(group)}</h3></div></div>
      <div class="field-grid">${PARAMETERS.filter(item => item.group === group).map(spec => {
        const shown = value[spec.key] / (spec.scale || 1);
        const supported = !state?.config || parameterSupported(spec.key, state.config.version);
        const editable = state?.ground === true && supported;
        const hint = supported ? spec.hint : `需要固件 v${spec.key.startsWith('model') ? 5 : 6}；当前设备为 v${state!.config!.version}。`;
        return `<label class="field" title="${escapeHtml(spec.hint)}"><span class="field-label">${escapeHtml(spec.label)}${spec.unit ? `<em>${escapeHtml(spec.unit)}</em>` : ''}</span>
          ${spec.options ? `<select data-key="${spec.key}" aria-describedby="field-error-${spec.key}" ${editable ? '' : 'disabled'}>${spec.options.map(option => `<option value="${option.value}" ${shown === option.value ? 'selected' : ''}>${escapeHtml(option.label)}</option>`).join('')}</select>` :
          `<input data-key="${spec.key}" type="number" value="${Number(shown.toFixed(6))}" min="${spec.min}" max="${spec.max}" step="${spec.step}" aria-describedby="field-error-${spec.key}" ${editable ? '' : 'disabled'} />`}
          <small>${escapeHtml(hint)}</small><small id="field-error-${spec.key}" class="field-error"></small></label>`;
      }).join('')}</div></section>`).join('')}</div>
      <aside class="config-aside"><section class="panel sticky-panel"><div class="eyebrow">CHECKLIST</div><h3>写入前确认</h3>
      <ul class="check-list"><li>设备位于 GROUND，鸭翼舵机保持中位。</li><li>机械端点和方向已按机体实测。</li><li>新控制律先设置配平与参考角，再逐步增加补偿。</li><li>写入后重新读取确认；保存不会自动进入 RUN。</li></ul>
      </section></aside></div>`}`;
  document.querySelectorAll<HTMLInputElement | HTMLSelectElement>('[data-key]').forEach(input => input.addEventListener('input', () => {
    if (!draft) return;
    const spec = PARAMETERS.find(item => item.key === input.dataset.key)!;
    if (!input.value.trim() || (input instanceof HTMLInputElement && input.validity.badInput)) {
      invalidInputs.set(spec.key, `${spec.label}不能为空`);
      updateDraftState();
      return;
    }
    invalidInputs.delete(spec.key);
    draft[spec.key] = Number(input.value) * (spec.scale || 1);
    updateDraftState();
  }));
  document.querySelector<HTMLElement>('#config-errors')?.addEventListener('click', event => {
    const key = (event.target as HTMLElement).closest<HTMLButtonElement>('[data-focus-key]')?.dataset.focusKey;
    const field = key ? document.querySelector<HTMLInputElement | HTMLSelectElement>(`[data-key="${key}"]`) : null;
    field?.scrollIntoView({ block: 'center', behavior: 'smooth' });
    field?.focus();
  });
  document.querySelector<HTMLButtonElement>('#read-config')?.addEventListener('click', () => void action(async () => {
    if (!await confirmDiscardDraft('重新读取设备配置')) return;
    const snapshot = await session!.refresh(); replaceDraft(snapshot.config); notice('配置已重新读取。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#apply-config')?.addEventListener('click', () => void action(async () => {
    if (!await confirmAction('确认应用参数？', `设备将立即使用以下参数；本次操作不会写入 EEPROM。\n\n${describeChanges(session!.state.config!.config, draft!)}`, '应用并回读')) return;
    let snapshot;
    try { snapshot = await session!.apply(draft!); }
    catch (error) {
      writeWarning = '写入中断，部分参数可能已应用。请先重新读取设备配置，再核对草稿与设备值。';
      renderPage(); throw error;
    }
    replaceDraft(snapshot.config);
    notice('参数已应用并回读，尚未持久保存。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#save-config')?.addEventListener('click', () => void action(async () => {
    if (!await confirmAction('确认保存到设备？', `以下参数将应用并写入 EEPROM。\n\n${describeChanges(session!.state.config!.config, draft!)}`, '保存并回读')) return;
    let snapshot;
    try { snapshot = await session!.save(draft!); }
    catch (error) {
      writeWarning = '保存中断，设备运行参数和 EEPROM 可能不同。请重新读取并核对后再保存。';
      renderPage(); throw error;
    }
    replaceDraft(snapshot.config);
    notice('参数已保存并回读。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#restore-defaults')?.addEventListener('click', () => void action(async () => {
    if (!await confirmAction('确认恢复默认参数？', `设备当前参数会立即改为默认值；此操作暂不写入 EEPROM。\n\n${describeChanges(session!.state.config!.config, DEFAULT_CONFIG)}`, '恢复默认')) return;
    const snapshot = await session!.defaults(); replaceDraft(snapshot.config);
    notice('设备已恢复默认参数，本次会话尚未保存。'); renderPage();
  }));
  updateDraftState();
}

function updateDraftState(): void {
  if (page !== 'config' || !draft) return;
  const state = session?.state;
  const issues: ConfigIssue[] = [
    ...Array.from(invalidInputs, ([key, message]) => ({ keys: [key], message })),
    ...configIssues(normalizeConfig(draft)),
  ];
  if (state?.config) for (const key of changedKeys(state.config.config, normalizeConfig(draft))) {
    if (!parameterSupported(key, state.config.version)) {
      issues.push({ keys: [key], message: `${PARAMETERS.find(item => item.key === key)!.label}不受设备固件 v${state.config.version} 支持` });
    }
  }
  const count = state?.config ? changedKeys(state.config.config, normalizeConfig(draft)).length : 0;
  const label = issues.length ? '草稿需修正' : !state?.connected ? '离线草稿' : count ? `${count} 项待应用` :
    state.saved === true ? '本会话已确认保存' : state.saved === false ? '已应用，尚未保存' : '设备持久保存状态未知';
  const detail = issues.length ? issues[0].message : state?.ground === true ? '仅在 GROUND 状态可写' : '当前为只读状态';
  const set = (id: string, text: string) => { const el = document.querySelector<HTMLElement>(`#${id}`); if (el) el.textContent = text; };
  set('draft-state', label); set('draft-detail', detail);
  const validation = document.querySelector<HTMLElement>('#config-errors');
  if (validation) validation.innerHTML = issues.length ? `<strong>请修正 ${issues.length} 项</strong>${issues.map(issue =>
    `<button type="button" class="validation-link" data-focus-key="${issue.keys[0]}">${escapeHtml(issue.message)}</button>`).join('')}` : '';
  document.querySelectorAll<HTMLInputElement | HTMLSelectElement>('[data-key]').forEach(input => {
    const key = input.dataset.key as ConfigKey;
    const issue = issues.find(item => item.keys.includes(key));
    const error = document.querySelector<HTMLElement>(`#field-error-${key}`);
    if (error) error.textContent = issue?.message || '';
    input.setAttribute('aria-invalid', issue ? 'true' : 'false');
  });
  const apply = document.querySelector<HTMLButtonElement>('#apply-config');
  const save = document.querySelector<HTMLButtonElement>('#save-config');
  if (apply) apply.disabled = busy || state?.ground !== true || !!issues.length || count === 0;
  if (save) save.disabled = busy || state?.ground !== true || !!issues.length || (count === 0 && state?.saved === true);
}

function readProfiles(): Profile[] {
  try {
    const parsed = JSON.parse(localStorage.getItem('easycanad-profiles-v2') || '[]');
    return Array.isArray(parsed) ? parsed.flatMap(item => {
      try { return [profileFromJson(item)]; } catch { return []; }
    }) : [];
  } catch { return []; }
}
function saveProfiles(profiles: Profile[]): void { localStorage.setItem('easycanad-profiles-v2', JSON.stringify(profiles)); }

async function loadProfile(profile: Profile): Promise<void> {
  if (!await confirmDiscardDraft(`载入“${profile.name}”`)) return;
  const next = { ...profile.config };
  const current = session?.state.config;
  const skipped: string[] = [];
  if (current) for (const spec of PARAMETERS) {
    if (!parameterSupported(spec.key, current.version) && next[spec.key] !== current.config[spec.key]) {
      next[spec.key] = current.config[spec.key];
      skipped.push(spec.label);
    }
  }
  replaceDraft(next);
  page = 'config'; renderPage();
  notice(skipped.length ? `已载入草稿；设备固件不支持的 ${skipped.length} 项已保留设备原值。` : `已将“${profile.name}”载入草稿。`);
}

function renderProfiles(): void {
  const profiles = readProfiles();
  const hasSource = !!(draft || session?.state.config);
  const lastDeleted = deletedProfiles.at(-1);
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">AIRFRAME / PROFILES</div><h1>机型档案</h1><p>每架飞机保存一份完整参数。导入旧版机型 JSON 时自动转换为新格式。</p></div>
    <div class="two-column profiles-layout"><section class="panel"><div class="panel-title"><div><div class="eyebrow">LOCAL LIBRARY</div><h3>本机保存的档案</h3></div><span class="pill">${profiles.length} 份</span></div>
      ${lastDeleted ? `<div class="undo-banner">已删除“${escapeHtml(lastDeleted.profile.name)}”${deletedProfiles.length > 1 ? `，共 ${deletedProfiles.length} 项可撤销` : ''}。<button id="undo-profile-delete" class="button text">撤销删除</button></div>` : ''}
      <div id="profile-list" class="profile-list">${profiles.length ? profiles.map((profile, index) => `<div class="profile-item"><div class="profile-avatar">${escapeHtml(profile.name.slice(0, 1).toUpperCase())}</div><div><strong>${escapeHtml(profile.name)}</strong><small>${escapeHtml(profile.notes || '完整控制参数')}</small></div><button class="button text" data-load-profile="${index}">载入草稿</button><button class="button text" data-copy-profile="${index}">复制</button><button class="button text" data-export-profile="${index}">导出</button><button class="button text danger" data-delete-profile="${index}">删除</button></div>`).join('') : '<div class="empty-mini">还没有本地档案。可以从设备配置保存，或导入已有 JSON。</div>'}</div>
    </section><section class="panel"><div class="panel-title"><div><div class="eyebrow">CREATE / IMPORT</div><h3>保存当前参数</h3></div></div>
      <label class="field"><span class="field-label">机型名称</span><input id="profile-name" placeholder="例如：歼-10 一号机" maxlength="80" /></label>
      <label class="field"><span class="field-label">备注</span><textarea id="profile-notes" placeholder="电池、舵机、机械版本或测试日期" maxlength="500"></textarea></label>
      <div class="button-row wrap"><button id="save-profile" class="button primary" ${hasSource ? '' : 'disabled'}>保存到本机</button><button id="export-profile" class="button secondary" ${hasSource ? '' : 'disabled'}>导出 JSON</button><label class="button secondary file-button">导入 JSON<input id="import-profile" type="file" accept=".json,application/json" hidden /></label></div>
      <p class="subtle-note">${hasSource ? '当前参数来自设备或已载入的草稿。' : '先连接设备或载入档案，才能保存当前参数。'}本地档案只保存在当前浏览器；导入只加入档案库，载入才会修改草稿。</p>
    </section></div>`;
  document.querySelector<HTMLButtonElement>('#undo-profile-delete')?.addEventListener('click', () => {
    const deleted = deletedProfiles.pop(); if (!deleted) return;
    profiles.splice(Math.min(deleted.index, profiles.length), 0, deleted.profile);
    saveProfiles(profiles); renderPage(); notice('机型档案已恢复。');
  });
  document.querySelectorAll<HTMLButtonElement>('[data-load-profile]').forEach(button => button.addEventListener('click', () => void loadProfile(profiles[Number(button.dataset.loadProfile)])));
  document.querySelectorAll<HTMLButtonElement>('[data-copy-profile]').forEach(button => button.addEventListener('click', () => {
    const original = profiles[Number(button.dataset.copyProfile)];
    const base = `${original.name.slice(0, 68)} - 副本`;
    let name = base, suffix = 2;
    while (profiles.some(item => item.name.toLocaleLowerCase() === name.toLocaleLowerCase())) name = `${base} (${suffix++})`;
    const copy = { ...original, name, config: { ...original.config } };
    profiles.push(copy); saveProfiles(profiles); renderPage(); notice(`已复制“${original.name}”。`);
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-export-profile]').forEach(button => button.addEventListener('click', () => {
    exportProfile(profiles[Number(button.dataset.exportProfile)]);
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-delete-profile]').forEach(button => button.addEventListener('click', () => {
    const index = Number(button.dataset.deleteProfile);
    deletedProfiles.push({ profile: profiles[index], index });
    profiles.splice(index, 1); saveProfiles(profiles); renderPage();
  }));
  const currentProfile = (): Profile => {
    const name = document.querySelector<HTMLInputElement>('#profile-name')!.value.trim();
    if (!name) throw new Error('请填写机型名称');
    if (profiles.some(profile => profile.name.toLocaleLowerCase() === name.toLocaleLowerCase())) throw new Error('机型名称已存在，请换一个名称');
    const source = draft ?? session?.state.config?.config;
    if (!source) throw new Error('先连接设备或载入档案，才能保存当前参数');
    if (invalidInputs.size) throw new Error('草稿有未填写的参数，请先修正');
    const config = normalizeConfig(source);
    const errors = configErrors(config); if (errors.length) throw new Error(errors.join('；'));
    return { format: 'easycanad-profile', version: 2, name,
      notes: document.querySelector<HTMLTextAreaElement>('#profile-notes')!.value.trim(), config };
  };
  document.querySelector<HTMLButtonElement>('#save-profile')!.addEventListener('click', () => {
    try { const profile = currentProfile(); profiles.push(profile); saveProfiles(profiles); notice('机型档案已保存在本机。'); renderPage(); }
    catch (error) { notice(String(error), true); }
  });
  document.querySelector<HTMLButtonElement>('#export-profile')!.addEventListener('click', () => {
    try { exportProfile(currentProfile()); }
    catch (error) { notice(String(error), true); }
  });
  document.querySelector<HTMLInputElement>('#import-profile')!.addEventListener('change', event => void action(async () => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0]; if (!file) return;
    try {
      const profile = profileFromJson(JSON.parse(await file.text()), draft ?? session?.state.config?.config ?? DEFAULT_CONFIG);
      const base = profile.name;
      let suffix = 2;
      while (profiles.some(item => item.name.toLocaleLowerCase() === profile.name.toLocaleLowerCase())) {
        const ending = ` (${suffix++})`;
        profile.name = `${base.slice(0, 80 - ending.length)}${ending}`;
      }
      profiles.push(profile); saveProfiles(profiles);
      notice(`已导入“${profile.name}”；点击载入草稿后才会修改当前参数。`); renderPage();
    } finally { input.value = ''; }
  }));
}

function exportProfile(profile: Profile): void {
  download(`${profile.name.replace(/[\\/:*?"<>|]/g, '_')}.json`, JSON.stringify(profile, null, 2) + '\n', 'application/json');
}

function recordingText(): string {
  if (!session?.state.connected) return session?.state.lines.length || lastRecording?.lines.length ?
    '连接已断开；本次记录仍可导出。' : '连接设备后可开始前台记录。';
  if (!session.state.recording && session.state.lines.length) return '本次记录仅保留在当前页面，关闭前请先导出。';
  if (session.state.kind === 'simulation') return '模拟数据仅用于界面演示；原始日志不能用于固件算法回放。';
  return '记录期间需保持页面打开和串口连接；导出前请先停止记录。';
}
function renderLogs(): void {
  const state = session?.state;
  const selectedLog = activeLog();
  const selectedTimes = activeLogTimes();
  const frames = selectedLog?.frames;
  const recorded = state?.lines.length ? state.lines : lastRecording?.lines;
  const recordedKind = state?.lines.length ? state.kind : lastRecording?.kind;
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">FLIGHT DATA / LOGS</div><h1>日志与曲线</h1><p>查看实时读数，记录原始串口数据，或导入 v4/v5/v6 日志进行时间轴浏览。</p></div>
    <div class="workflow-bar"><div><span class="workflow-dot ${state?.recording ? 'pulse' : ''}"></span><strong>${state?.recording ? '正在前台记录' : recorded?.length && !recordingExported ? '当前记录待导出' : logSource === 'imported' && imported ? '已导入日志' : '准备记录'}</strong><small>${recordingText()}</small></div>
      <div class="button-row wrap"><button id="record-toggle" class="button ${state?.recording ? 'secondary' : 'primary'}" ${state?.connected ? '' : 'disabled'}>${state?.recording ? '停止记录' : '开始记录'}</button>
      <button id="export-raw" class="button secondary" ${recorded?.length && recordedKind === 'serial' && !state?.recording ? '' : 'disabled'}>导出原始日志</button>
      <button id="export-csv" class="button secondary" ${selectedLog && !state?.recording ? '' : 'disabled'}>导出分析 CSV</button>
      <label class="button secondary file-button ${state?.recording ? 'is-disabled' : ''}">导入原始日志<input id="import-log" type="file" accept=".ecsv,.csv,.txt,text/plain" ${state?.recording ? 'disabled' : ''} hidden /></label></div></div>
    ${imported && (recorded?.length || liveFrames.length) ? `<div class="log-source-switch" role="group" aria-label="日志数据来源"><button data-log-source="live" class="button ${logSource === 'live' ? 'primary' : 'secondary'}">当前记录</button><button data-log-source="imported" class="button ${logSource === 'imported' ? 'primary' : 'secondary'}">导入记录</button></div>` : ''}
    <section class="panel chart-panel"><div class="panel-title"><div><div class="eyebrow">TIMELINE</div><h3>${selectedLog ? logSource === 'imported' ? '导入记录' : '当前记录' : '实时窗口'}</h3></div><span class="chart-legend"><i></i> 风标角 <i></i> 鸭翼舵令</span></div>
      <canvas id="log-chart" height="300" aria-label="风标角与鸭翼舵令曲线"></canvas>
      ${frames ? `<div class="timeline-control"><button id="play-log" class="button secondary">${playTimer ? '暂停' : '播放'}</button><label>速度 <select id="play-speed" aria-label="回放速度"><option value="0.5" ${playbackSpeed === 0.5 ? 'selected' : ''}>0.5×</option><option value="1" ${playbackSpeed === 1 ? 'selected' : ''}>1×</option><option value="2" ${playbackSpeed === 2 ? 'selected' : ''}>2×</option></select></label><span>第 <b id="cursor-index">${cursor + 1}</b> / ${frames.length} 帧</span><input id="log-cursor" aria-label="日志时间轴" type="range" min="0" max="${frames.length - 1}" value="${cursor}" /><span id="cursor-time">${fmt((selectedTimes[cursor] || 0) / 1_000_000, 2)} s</span></div><div id="cursor-values" class="cursor-values"></div>` : !liveFrames.length ? '<div class="chart-empty">连接设备或导入日志后显示曲线。</div>' : ''}</section>
    <div class="stat-grid log-stats"><div class="stat-card"><span>控制状态</span><strong id="log-live-state">—</strong><small>当前或最新帧</small></div>
      <div class="stat-card"><span>已记录</span><strong id="log-live-count">—</strong><small>前台原始帧</small></div>
      <div class="stat-card"><span>导入帧数</span><strong>${imported?.frames.length ?? '—'}</strong><small>${imported ? `序号缺口 ${imported.gaps}` : '未导入文件'}</small></div>
      <div class="stat-card"><span>日志协议</span><strong>${selectedLog ? `v${selectedLog.config.version}` : state?.config ? `v${state.config.version}` : '—'}</strong><small>可读取 v4 / v5 / v6</small></div></div>`;
  document.querySelectorAll<HTMLButtonElement>('[data-log-source]').forEach(button => button.addEventListener('click', () => {
    stopPlayback(); logSource = button.dataset.logSource as 'live' | 'imported';
    cursor = Math.max(0, (activeLog()?.frames.length || 1) - 1); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#record-toggle')?.addEventListener('click', () => void action(async () => {
    if (session!.state.recording) {
      session!.stopRecording(); parseCurrentRecording(session!.state.lines);
      notice('已停止记录，可导出文件。');
    }
    else {
      if (recorded?.length && !recordingExported && !await confirmAction('开始新记录？', '上一段记录尚未导出。开始新记录会清空该段数据，请先导出，或确认放弃。', '放弃并开始')) return;
      lastRecording = undefined; currentRecording = undefined; currentRecordingTimes = [];
      await session!.startRecording(); recordingExported = false;
      logSource = 'live'; stopPlayback(); notice('已开始前台记录。');
    }
    renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#export-raw')?.addEventListener('click', () => {
    if (!recorded || recordedKind !== 'serial') return;
    download(`easycanad-${new Date().toISOString().replace(/[:.]/g, '-')}.ecsv`, '#EasyCanadLog\n' + recorded.join('\n') + '\n', 'text/plain');
    recordingExported = true;
  });
  document.querySelector<HTMLButtonElement>('#export-csv')?.addEventListener('click', () => {
    try {
      if (!selectedLog) return;
      download('easycanad-frames.csv', framesToCsv(selectedLog.frames), 'text/csv');
      if (logSource === 'live' && recordedKind === 'simulation') recordingExported = true;
    } catch (error) { notice(String(error), true); }
  });
  document.querySelector<HTMLInputElement>('#import-log')?.addEventListener('change', event => void action(async () => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0]; if (!file) return;
    try {
      const raw = await file.text();
      if (raw.trimStart().startsWith('sequence,timeUs,elapsedUs')) throw new Error('分析 CSV 缺少设备配置记录；请导入包含 C/D 行的原始日志');
      imported = parseLog(raw); importedTimes = frameTimelineUs(imported.frames);
      logSource = 'imported';
      cursor = imported.frames.length - 1;
      stopPlayback();
      notice(`已导入 ${imported.frames.length} 帧。`); renderPage();
    } finally { input.value = ''; }
  }));
  document.querySelector<HTMLInputElement>('#log-cursor')?.addEventListener('input', event => {
    cursor = Number((event.target as HTMLInputElement).value);
    if (playTimer) { stopPlayback(); renderPage(); return; }
    updateCursor();
  });
  document.querySelector<HTMLSelectElement>('#play-speed')?.addEventListener('change', event => {
    playbackSpeed = Number((event.target as HTMLSelectElement).value);
    if (playTimer) { stopPlayback(); renderPage(); }
  });
  document.querySelector<HTMLButtonElement>('#play-log')?.addEventListener('click', () => {
    if (playTimer) { stopPlayback(); renderPage(); return; }
    if (!selectedLog) return;
    if (cursor >= selectedLog.frames.length - 1) cursor = 0;
    const startUs = selectedTimes[cursor];
    const startedAt = performance.now();
    playTimer = setInterval(() => {
      if (activeLog() !== selectedLog || page !== 'logs') {
        stopPlayback(); renderPage(); return;
      }
      const targetUs = startUs + (performance.now() - startedAt) * 1000 * playbackSpeed;
      let low = cursor, high = selectedLog.frames.length - 1;
      while (low < high) {
        const mid = Math.ceil((low + high) / 2);
        if (selectedTimes[mid] <= targetUs) low = mid;
        else high = mid - 1;
      }
      cursor = low;
      updateCursor();
      if (cursor >= selectedLog.frames.length - 1) { stopPlayback(); renderPage(); }
    }, 50);
    renderPage();
  });
  updateLive(); if (selectedLog) updateCursor(); else drawChart();
}

function updateCursor(): void {
  const selectedLog = activeLog();
  const frame = selectedLog?.frames[cursor];
  if (!frame) return;
  const set = (id: string, value: string) => { const node = document.querySelector<HTMLElement>(`#${id}`); if (node) node.textContent = value; };
  set('cursor-index', String(cursor + 1));
  set('cursor-time', `${fmt((activeLogTimes()[cursor] || 0) / 1_000_000, 2)} s`);
  set('log-live-state', STATE_NAMES[frame.state]);
  set('cursor-values', `风标 ${fmt(relativeAoa(frame, selectedLog!.config.zeroDeg))}° · 鸭翼舵令 ${fmt(frame.commandDeg)}° · 舵机 ${frame.servoUs} μs · ${STATE_NAMES[frame.state]} · 丢帧 ${frame.dropped}`);
  drawChart();
}

function drawChart(): void {
  const canvas = document.querySelector<HTMLCanvasElement>('#log-chart'); if (!canvas) return;
  const width = Math.max(300, canvas.clientWidth);
  const height = 300;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
  const context = canvas.getContext('2d')!;
  context.scale(dpr, dpr);
  context.clearRect(0, 0, width, height);
  const selectedLog = activeLog();
  const frames = selectedLog?.frames ?? liveFrames;
  const times = selectedLog ? activeLogTimes() : frameTimelineUs(frames);
  const zero = selectedLog?.config.zeroDeg ?? session?.state.config?.zeroDeg ?? 0;
  const left = 44, right = width - 16, top = 20, bottom = height - 32;
  context.strokeStyle = '#d8e2e5'; context.lineWidth = 1;
  for (let index = 0; index <= 4; index++) {
    const y = top + (bottom - top) * index / 4;
    context.beginPath(); context.moveTo(left, y); context.lineTo(right, y); context.stroke();
  }
  if (frames.length < 2) return;
  const limit = Math.max(20, ...frames.filter((_, index) => index % Math.max(1, Math.ceil(frames.length / 500)) === 0)
    .flatMap(frame => [Math.abs(frame.commandDeg), Math.abs(relativeAoa(frame, zero))]));
  context.fillStyle = '#748790'; context.font = '12px system-ui';
  context.fillText(`${fmt(limit, 0)}°`, 2, top + 4); context.fillText('0°', 17, (top + bottom) / 2 + 4);
  context.fillText(`-${fmt(limit, 0)}°`, 0, bottom + 4);
  const durationUs = times.at(-1) || 0;
  context.fillText('0 s', left, height - 8);
  context.fillText(`${fmt(durationUs / 2_000_000, 1)} s`, (left + right) / 2 - 14, height - 8);
  context.fillText(`${fmt(durationUs / 1_000_000, 1)} s`, right - 30, height - 8);
  const draw = (color: string, select: (frame: DataFrame) => number) => {
    context.strokeStyle = color; context.lineWidth = 2; context.beginPath();
    const step = Math.max(1, Math.ceil(frames.length / (right - left)));
    for (let i = 0; i < frames.length; i += step) {
      const x = left + (right - left) * (durationUs ? times[i] / durationUs : i / (frames.length - 1));
      const y = (top + bottom) / 2 - select(frames[i]) / limit * (bottom - top) / 2;
      if (i === 0) context.moveTo(x, y); else context.lineTo(x, y);
    }
    context.stroke();
  };
  draw('#1ca88c', frame => relativeAoa(frame, zero));
  draw('#ed9751', frame => frame.commandDeg);
  if (selectedLog) {
    const x = left + (right - left) * (durationUs ? times[cursor] / durationUs : cursor / (frames.length - 1));
    context.strokeStyle = '#173746'; context.setLineDash([4, 4]); context.beginPath();
    context.moveTo(x, top); context.lineTo(x, bottom); context.stroke(); context.setLineDash([]);
  }
}

window.addEventListener('resize', () => { if (page === 'logs') drawChart(); });
window.setInterval(() => { if (session?.state.connected) updateTop(); }, 1000);
window.addEventListener('beforeunload', event => {
  if (!hasPendingDraft() && !session?.state.recording && (recordingExported || !(session?.state.lines.length || lastRecording?.lines.length))) return;
  event.preventDefault();
  event.returnValue = '';
});
if (import.meta.env.PROD && 'serviceWorker' in navigator) {
  window.addEventListener('load', () => void navigator.serviceWorker.register('./sw.js'));
}
renderPage();
