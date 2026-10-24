import './style.css';
import { changedKeys, configErrors, DEFAULT_CONFIG, normalizeConfig, PARAMETERS, type ControlConfig } from './protocol/config';
import { DeviceSession } from './protocol/deviceSession';
import { framesToCsv, parseLog, STATE_NAMES, type DataFrame, type ParsedLog } from './protocol/log';
import { profileFromJson, type Profile } from './protocol/profile';
import { hasWebSerial, WebSerialTransport } from './transport/lineTransport';
import { SimulatedDevice } from './transport/simulatedDevice';

type Page = 'device' | 'config' | 'profiles' | 'logs';
const app = document.querySelector<HTMLDivElement>('#app')!;
let page: Page = 'device';
let session: DeviceSession | undefined;
let simulation: SimulatedDevice | undefined;
let unsubscribe: (() => void)[] = [];
let draft: ControlConfig | undefined;
let imported: ParsedLog | undefined;
let liveFrames: DataFrame[] = [];
let lastRecording: { kind: 'serial' | 'simulation'; lines: string[]; frames: number } | undefined;
let cursor = 0;
let playTimer: ReturnType<typeof setInterval> | undefined;
let busy = false;
let toastTimer: ReturnType<typeof setTimeout> | undefined;

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character]!);
}
function fmt(value: number, digits = 1): string { return Number.isFinite(value) ? value.toFixed(digits) : '—'; }
function download(name: string, content: string, type: string): void {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const link = document.createElement('a');
  link.href = url; link.download = name; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function notice(message: string, error = false): void {
  const element = document.querySelector<HTMLDivElement>('#notice')!;
  element.textContent = message;
  element.className = `notice show${error ? ' error' : ''}`;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => element.className = 'notice', 5500);
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
        <div class="top-actions"><span id="connection-chip" class="chip">未连接</span><span id="mode-chip" class="chip subtle">—</span></div>
      </header>
      <main id="page-content"></main>
      <nav class="mobile-nav" aria-label="手机导航">
        <button data-page="device">◉<span>设备</span></button><button data-page="config">◧<span>调参</span></button>
        <button data-page="profiles">▤<span>机型</span></button><button data-page="logs">⌁<span>日志</span></button>
      </nav>
    </div>
    <div id="notice" class="notice" role="status"></div>
  </div>`;
document.querySelectorAll<HTMLButtonElement>('[data-page]').forEach(button => button.addEventListener('click', () => {
  stopPlayback();
  page = button.dataset.page as Page; renderPage();
}));

function stopPlayback(): void {
  if (playTimer) clearInterval(playTimer);
  playTimer = undefined;
}

function updateTop(): void {
  const state = session?.state;
  const connection = document.querySelector<HTMLElement>('#connection-chip')!;
  connection.textContent = state?.connected ? (state.kind === 'simulation' ? '模拟设备' : '已连接') : '未连接';
  connection.className = `chip ${state?.connected ? 'online' : ''}`;
  const mode = document.querySelector<HTMLElement>('#mode-chip')!;
  mode.textContent = state?.ground === true ? 'GROUND · 可配置' : state?.ground === false ? 'RUN · 只读' : '—';
  mode.className = `chip subtle ${state?.ground === true ? 'ground' : ''}`;
  document.querySelector<HTMLElement>('#page-name')!.textContent = ({ device: '设备', config: '调参', profiles: '机型', logs: '日志' })[page];
  document.querySelectorAll<HTMLButtonElement>('[data-page]').forEach(button => button.classList.toggle('active', button.dataset.page === page));
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
    next.subscribeFrames(frame => { liveFrames.push(frame); if (liveFrames.length > 1200) liveFrames.shift(); })];
  try {
    const snapshot = await next.connect();
    draft = { ...snapshot.config };
    liveFrames = [];
    notice(kind === 'simulation' ? '已连接模拟设备。模拟读数仅供界面验证。' : '已连接，配置已从设备读取。');
  } catch (error) {
    await disconnect();
    throw error;
  }
  renderPage();
}
async function disconnect(): Promise<void> {
  unsubscribe.forEach(remove => remove()); unsubscribe = [];
  if (session?.state.lines.length) {
    lastRecording = { kind: session.state.kind,
      lines: [...session.state.lines], frames: session.state.recordedFrames };
    if (!imported) {
      try {
        imported = parseLog('#EasyCanadLog\n' + lastRecording.lines.join('\n'));
        cursor = imported.frames.length - 1;
      } catch { /* An incomplete recording can still be exported as raw lines. */ }
    }
  }
  if (session) await session.close();
  session = undefined; simulation = undefined; draft = undefined; liveFrames = [];
  renderPage();
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
      <p>${state?.connected ? '配置与状态来自当前连接。运行状态下仅允许查看与记录。' : '先连接 USB 串口，或使用模拟设备体验完整地面配置流程。'}</p>
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
      <div><span>油门输入</span><b id="detail-throttle">—</b></div><div><span>协议版本</span><b id="detail-version">—</b></div></div>
      ${state?.ground === true ? '<div class="panel-actions"><button id="enter-run" class="button secondary">手动进入 RUN</button><p class="subtle-note">先拔掉 D5 跳线，遥控置手动且俯仰回中；设备会自行检查条件。</p></div>' : ''}
      ${simulation ? '<div class="panel-actions"><button id="toggle-sim-mode" class="button secondary">模拟切换 GROUND / RUN</button></div>' : ''}</section>
      <section class="panel"><div class="panel-title"><div><div class="eyebrow">CALIBRATION</div><h3>风标零位</h3></div><span id="zero-value" class="pill">—</span></div>
      <p id="cal-status" class="body-copy">标定状态 —</p>
      <p class="body-copy">仅在地面配置状态、遥控手动且俯仰回中时，长按设备 D4 按钮约 1.5 秒。标定采样需要风标保持稳定。</p>
      <p class="subtle-note">串口连接可能使 Nano 重启。进入 RUN 仍由地面跳线和设备命令条件控制；本界面不会自动进入 RUN。</p></section></div>`;
  document.querySelector<HTMLButtonElement>('#connect-serial')?.addEventListener('click', () => void action(() => connect('serial')));
  document.querySelector<HTMLButtonElement>('#connect-sim')?.addEventListener('click', () => void action(() => connect('simulation')));
  document.querySelector<HTMLButtonElement>('#disconnect')?.addEventListener('click', () => void action(disconnect));
  document.querySelector<HTMLButtonElement>('#refresh')?.addEventListener('click', () => void action(async () => {
    const snapshot = await session!.refresh(); draft = { ...snapshot.config }; notice('已从设备重新读取配置。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#enter-run')?.addEventListener('click', () => void action(async () => {
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
  const frame = state?.live;
  const set = (id: string, value: string) => { const node = document.querySelector<HTMLElement>(`#${id}`); if (node) node.textContent = value; };
  set('metric-state', frame ? STATE_NAMES[frame.state] : '—');
  set('metric-state-detail', frame ? `控制周期 ${fmt(frame.elapsedUs / 1000, 0)} ms` : '等待设备输入');
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
  set('device-mode-tag', state?.ground === true ? 'GROUND' : state?.ground === false ? 'RUN' : '—');
  set('zero-value', state?.config ? `${fmt(state.config.zeroDeg, 2)}°` : '—');
  const calStatus = ['空闲', '采样中', '已保存', '拒绝或失败'][frame?.calibrationStatus ?? -1] ?? '—';
  set('cal-status', `标定状态 ${calStatus}${frame && !(frame.flags & 8) ? ' · 零位无效' : ''}`);
  const selected = imported?.frames[cursor];
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
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">PARAMETERS / CONFIGURATION</div><h1>控制参数</h1><p>先修改本地草稿，再应用并从设备回读。保存会写入设备的 EEPROM。</p></div>
    <div class="workflow-bar"><div><span class="workflow-dot"></span><strong id="draft-state">${state?.connected ? '已读取设备' : '未连接设备'}</strong><small id="draft-detail">${state?.ground === true ? '地面配置可写' : '连接 GROUND 设备后可编辑'}</small></div>
      <div class="button-row wrap"><button id="read-config" class="button secondary" ${state?.connected ? '' : 'disabled'}>重新读取</button>
      <button id="apply-config" class="button secondary" disabled>应用到设备</button><button id="save-config" class="button primary" disabled>保存到设备</button>
      ${state?.ground === true ? '<button id="restore-defaults" class="button secondary">恢复默认</button>' : ''}</div></div>
    ${!value ? '<div class="empty-state"><div class="empty-symbol">◧</div><h3>尚无设备配置</h3><p>连接控制器或模拟设备后，即可查看全部参数。</p></div>' :
      `<div class="config-layout"><div class="config-main">${groups.map(group => `<section class="panel config-group"><div class="panel-title"><div><div class="eyebrow">${escapeHtml(group === '鸭翼控制' ? 'CONTROL LAW' : group === '机型模型' ? 'AIRFRAME' : 'SETTINGS')}</div><h3>${escapeHtml(group)}</h3></div></div>
      <div class="field-grid">${PARAMETERS.filter(item => item.group === group).map(spec => {
        const shown = value[spec.key] / (spec.scale || 1);
        return `<label class="field" title="${escapeHtml(spec.hint)}"><span class="field-label">${escapeHtml(spec.label)}${spec.unit ? `<em>${escapeHtml(spec.unit)}</em>` : ''}</span>
          ${spec.options ? `<select data-key="${spec.key}" ${state?.ground === true ? '' : 'disabled'}>${spec.options.map(option => `<option value="${option.value}" ${shown === option.value ? 'selected' : ''}>${escapeHtml(option.label)}</option>`).join('')}</select>` :
          `<input data-key="${spec.key}" type="number" value="${Number(shown.toFixed(6))}" min="${spec.min}" max="${spec.max}" step="${spec.step}" ${state?.ground === true ? '' : 'disabled'} />`}
          <small>${escapeHtml(spec.hint)}</small></label>`;
      }).join('')}</div></section>`).join('')}</div>
      <aside class="config-aside"><section class="panel sticky-panel"><div class="eyebrow">CHECKLIST</div><h3>写入前确认</h3>
      <ul class="check-list"><li>设备位于 GROUND，鸭翼舵机保持中位。</li><li>机械端点和方向已按机体实测。</li><li>新控制律先设置配平与参考角，再逐步增加补偿。</li><li>写入后重新读取确认；保存不会自动进入 RUN。</li></ul>
      <div id="config-errors" class="validation"></div></section></aside></div>`}`;
  document.querySelectorAll<HTMLInputElement | HTMLSelectElement>('[data-key]').forEach(input => input.addEventListener('input', () => {
    if (!draft) return;
    const spec = PARAMETERS.find(item => item.key === input.dataset.key)!;
    draft[spec.key] = Number(input.value) * (spec.scale || 1);
    updateDraftState();
  }));
  document.querySelector<HTMLButtonElement>('#read-config')?.addEventListener('click', () => void action(async () => {
    const snapshot = await session!.refresh(); draft = { ...snapshot.config }; notice('配置已重新读取。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#apply-config')?.addEventListener('click', () => void action(async () => {
    const snapshot = await session!.apply(draft!); draft = { ...snapshot.config };
    notice('参数已应用并回读，尚未持久保存。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#save-config')?.addEventListener('click', () => void action(async () => {
    const snapshot = await session!.save(draft!); draft = { ...snapshot.config };
    notice('参数已保存并回读。'); renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#restore-defaults')?.addEventListener('click', () => void action(async () => {
    const snapshot = await session!.defaults(); draft = { ...snapshot.config };
    notice('设备已恢复默认参数，本次会话尚未保存。'); renderPage();
  }));
  updateDraftState();
}

function updateDraftState(): void {
  if (page !== 'config' || !draft) return;
  const state = session?.state;
  const errors = configErrors(normalizeConfig(draft));
  const count = state?.config ? changedKeys(state.config.config, normalizeConfig(draft)).length : 0;
  const label = errors.length ? '草稿需修正' : count ? `${count} 项待应用` : state?.saved ? '设备配置已保存' : '已应用，尚未保存';
  const detail = errors.length ? errors[0] : state?.ground === true ? '仅在 GROUND 状态可写' : '当前为只读状态';
  const set = (id: string, text: string) => { const el = document.querySelector<HTMLElement>(`#${id}`); if (el) el.textContent = text; };
  set('draft-state', label); set('draft-detail', detail);
  const validation = document.querySelector<HTMLElement>('#config-errors');
  if (validation) validation.innerHTML = errors.length ? `<strong>参数检查</strong>${errors.map(message => `<p>${escapeHtml(message)}</p>`).join('')}` : '<span class="valid-dot"></span> 参数范围检查通过';
  const apply = document.querySelector<HTMLButtonElement>('#apply-config');
  const save = document.querySelector<HTMLButtonElement>('#save-config');
  if (apply) apply.disabled = busy || state?.ground !== true || !!errors.length || count === 0;
  if (save) save.disabled = busy || state?.ground !== true || !!errors.length || (count === 0 && !!state?.saved);
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

function renderProfiles(): void {
  const profiles = readProfiles();
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">AIRFRAME / PROFILES</div><h1>机型档案</h1><p>每架飞机保存一份完整参数。导入旧版机型 JSON 时自动转换为新格式。</p></div>
    <div class="two-column profiles-layout"><section class="panel"><div class="panel-title"><div><div class="eyebrow">LOCAL LIBRARY</div><h3>本机保存的档案</h3></div><span class="pill">${profiles.length} 份</span></div>
      <div id="profile-list" class="profile-list">${profiles.length ? profiles.map((profile, index) => `<div class="profile-item"><div class="profile-avatar">${escapeHtml(profile.name.slice(0, 1).toUpperCase())}</div><div><strong>${escapeHtml(profile.name)}</strong><small>${escapeHtml(profile.notes || '完整控制参数')}</small></div><button class="button text" data-load-profile="${index}">载入草稿</button><button class="button text" data-copy-profile="${index}">复制</button><button class="button text" data-export-profile="${index}">导出</button><button class="button text danger" data-delete-profile="${index}">删除</button></div>`).join('') : '<div class="empty-mini">还没有本地档案。可以从设备配置保存，或导入已有 JSON。</div>'}</div>
    </section><section class="panel"><div class="panel-title"><div><div class="eyebrow">CREATE / IMPORT</div><h3>保存当前参数</h3></div></div>
      <label class="field"><span class="field-label">机型名称</span><input id="profile-name" placeholder="例如：歼-10 一号机" maxlength="80" /></label>
      <label class="field"><span class="field-label">备注</span><textarea id="profile-notes" placeholder="电池、舵机、机械版本或测试日期" maxlength="500"></textarea></label>
      <div class="button-row wrap"><button id="save-profile" class="button primary">保存到本机</button><button id="export-profile" class="button secondary">导出 JSON</button><label class="button secondary file-button">导入 JSON<input id="import-profile" type="file" accept=".json,application/json" hidden /></label></div>
      <p class="subtle-note">本地档案只保存在当前浏览器。导出的 JSON 可跨设备转移。载入档案只修改草稿，不会向控制器写入。</p>
    </section></div>`;
  document.querySelectorAll<HTMLButtonElement>('[data-load-profile]').forEach(button => button.addEventListener('click', () => {
    const profile = profiles[Number(button.dataset.loadProfile)]; draft = { ...profile.config };
    page = 'config'; renderPage(); notice(`已将“${profile.name}”载入草稿。`);
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-copy-profile]').forEach(button => button.addEventListener('click', () => {
    const original = profiles[Number(button.dataset.copyProfile)];
    const copy = { ...original, name: `${original.name.slice(0, 74)} - 副本`, config: { ...original.config } };
    profiles.push(copy); saveProfiles(profiles); renderPage(); notice(`已复制“${original.name}”。`);
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-export-profile]').forEach(button => button.addEventListener('click', () => {
    exportProfile(profiles[Number(button.dataset.exportProfile)]);
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-delete-profile]').forEach(button => button.addEventListener('click', () => {
    profiles.splice(Number(button.dataset.deleteProfile), 1); saveProfiles(profiles); renderPage();
  }));
  const currentProfile = (): Profile => {
    const name = document.querySelector<HTMLInputElement>('#profile-name')!.value.trim();
    if (!name) throw new Error('请填写机型名称');
    const config = normalizeConfig(draft ?? session?.state.config?.config ?? DEFAULT_CONFIG);
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
    const file = (event.target as HTMLInputElement).files?.[0]; if (!file) return;
    const profile = profileFromJson(JSON.parse(await file.text()), draft ?? session?.state.config?.config ?? DEFAULT_CONFIG);
    profiles.push(profile); saveProfiles(profiles); draft = { ...profile.config };
    notice(`已导入“${profile.name}”并载入草稿。`); renderPage();
  }));
}

function exportProfile(profile: Profile): void {
  download(`${profile.name.replace(/[\\/:*?"<>|]/g, '_')}.json`, JSON.stringify(profile, null, 2) + '\n', 'application/json');
}

function recordingText(): string {
  if (!session?.state.connected) return session?.state.lines.length || lastRecording?.lines.length ?
    '连接已断开；本次记录仍可导出。' : '连接设备后可开始前台记录。';
  if (session.state.kind === 'simulation') return '模拟数据仅用于界面演示；原始日志不能用于固件算法回放。';
  return '记录期间需保持页面打开和串口连接；导出前请先停止记录。';
}
function renderLogs(): void {
  const state = session?.state;
  const frames = imported?.frames;
  const recorded = state?.lines.length ? state.lines : lastRecording?.lines;
  const recordedKind = state?.lines.length ? state.kind : lastRecording?.kind;
  document.querySelector<HTMLElement>('#page-content')!.innerHTML = `
    <div class="page-heading"><div class="eyebrow">FLIGHT DATA / LOGS</div><h1>日志与曲线</h1><p>查看实时读数，记录原始串口数据，或导入 v4/v5/v6 日志进行时间轴浏览。</p></div>
    <div class="workflow-bar"><div><span class="workflow-dot ${state?.recording ? 'pulse' : ''}"></span><strong>${state?.recording ? '正在前台记录' : imported ? '已导入日志' : '准备记录'}</strong><small>${recordingText()}</small></div>
      <div class="button-row wrap"><button id="record-toggle" class="button ${state?.recording ? 'secondary' : 'primary'}" ${state?.connected ? '' : 'disabled'}>${state?.recording ? '停止记录' : '开始记录'}</button>
      <button id="export-raw" class="button secondary" ${recorded?.length && recordedKind === 'serial' ? '' : 'disabled'}>导出原始日志</button>
      <button id="export-csv" class="button secondary" ${imported || recorded?.length ? '' : 'disabled'}>导出 CSV</button>
      <label class="button secondary file-button">导入日志<input id="import-log" type="file" accept=".ecsv,.csv,.txt,text/plain" hidden /></label></div></div>
    <section class="panel chart-panel"><div class="panel-title"><div><div class="eyebrow">TIMELINE</div><h3>${imported ? '导入记录' : '实时窗口'}</h3></div><span class="chart-legend"><i></i> 风标角 <i></i> 鸭翼舵令</span></div>
      <canvas id="log-chart" height="300" aria-label="风标角与鸭翼舵令曲线"></canvas>
      ${frames ? `<div class="timeline-control"><button id="play-log" class="button secondary">${playTimer ? '暂停' : '播放'}</button><span>第 <b id="cursor-index">${cursor + 1}</b> / ${frames.length} 帧</span><input id="log-cursor" type="range" min="0" max="${frames.length - 1}" value="${cursor}" /><span id="cursor-time">${fmt(frames[cursor]?.timeUs / 1_000_000 || 0, 2)} s</span></div>` : !liveFrames.length ? '<div class="chart-empty">连接设备或导入日志后显示曲线。</div>' : ''}</section>
    <div class="stat-grid log-stats"><div class="stat-card"><span>控制状态</span><strong id="log-live-state">—</strong><small>当前或最新帧</small></div>
      <div class="stat-card"><span>已记录</span><strong id="log-live-count">—</strong><small>前台原始帧</small></div>
      <div class="stat-card"><span>导入帧数</span><strong>${frames?.length ?? '—'}</strong><small>${imported ? `序号缺口 ${imported.gaps}` : '未导入文件'}</small></div>
      <div class="stat-card"><span>日志协议</span><strong>${imported ? `v${imported.config.version}` : state?.config ? `v${state.config.version}` : '—'}</strong><small>可读取 v4 / v5 / v6</small></div></div>`;
  document.querySelector<HTMLButtonElement>('#record-toggle')?.addEventListener('click', () => void action(async () => {
    if (session!.state.recording) { session!.stopRecording(); notice('已停止记录，可导出文件。'); }
    else { lastRecording = undefined; await session!.startRecording(); imported = undefined; stopPlayback(); notice('已开始前台记录。'); }
    renderPage();
  }));
  document.querySelector<HTMLButtonElement>('#export-raw')?.addEventListener('click', () => {
    if (!recorded || recordedKind !== 'serial') return;
    download(`easycanad-${new Date().toISOString().replace(/[:.]/g, '-')}.ecsv`, '#EasyCanadLog\n' + recorded.join('\n') + '\n', 'text/plain');
  });
  document.querySelector<HTMLButtonElement>('#export-csv')?.addEventListener('click', () => {
    try {
      const log = imported ?? parseLog('#EasyCanadLog\n' + (recorded?.join('\n') ?? ''));
      download('easycanad-frames.csv', framesToCsv(log.frames), 'text/csv');
    } catch (error) { notice(String(error), true); }
  });
  document.querySelector<HTMLInputElement>('#import-log')?.addEventListener('change', event => void action(async () => {
    const file = (event.target as HTMLInputElement).files?.[0]; if (!file) return;
    imported = parseLog(await file.text()); cursor = imported.frames.length - 1;
    stopPlayback();
    notice(`已导入 ${imported.frames.length} 帧。`); renderPage();
  }));
  document.querySelector<HTMLInputElement>('#log-cursor')?.addEventListener('input', event => {
    cursor = Number((event.target as HTMLInputElement).value);
    updateCursor();
  });
  document.querySelector<HTMLButtonElement>('#play-log')?.addEventListener('click', () => {
    if (playTimer) { stopPlayback(); renderPage(); return; }
    if (!imported) return;
    if (cursor >= imported.frames.length - 1) cursor = 0;
    playTimer = setInterval(() => {
      if (!imported || page !== 'logs' || cursor >= imported.frames.length - 1) {
        stopPlayback(); renderPage(); return;
      }
      cursor = Math.min(imported.frames.length - 1, cursor + 10);
      updateCursor();
    }, 50);
    renderPage();
  });
  updateLive(); drawChart();
}

function updateCursor(): void {
  const frame = imported?.frames[cursor];
  if (!frame) return;
  const set = (id: string, value: string) => { const node = document.querySelector<HTMLElement>(`#${id}`); if (node) node.textContent = value; };
  set('cursor-index', String(cursor + 1));
  set('cursor-time', `${fmt(frame.timeUs / 1_000_000, 2)} s`);
  set('log-live-state', STATE_NAMES[frame.state]);
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
  const frames = imported?.frames ?? liveFrames;
  const zero = imported?.config.zeroDeg ?? session?.state.config?.zeroDeg ?? 0;
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
  const draw = (color: string, select: (frame: DataFrame) => number) => {
    context.strokeStyle = color; context.lineWidth = 2; context.beginPath();
    const step = Math.max(1, Math.ceil(frames.length / (right - left)));
    for (let i = 0; i < frames.length; i += step) {
      const x = left + (right - left) * i / (frames.length - 1);
      const y = (top + bottom) / 2 - select(frames[i]) / limit * (bottom - top) / 2;
      if (i === 0) context.moveTo(x, y); else context.lineTo(x, y);
    }
    context.stroke();
  };
  draw('#1ca88c', frame => relativeAoa(frame, zero));
  draw('#ed9751', frame => frame.commandDeg);
  if (imported) {
    const x = left + (right - left) * cursor / (frames.length - 1);
    context.strokeStyle = '#173746'; context.setLineDash([4, 4]); context.beginPath();
    context.moveTo(x, top); context.lineTo(x, bottom); context.stroke(); context.setLineDash([]);
  }
}

window.addEventListener('resize', () => { if (page === 'logs') drawChart(); });
if (import.meta.env.PROD && 'serviceWorker' in navigator) {
  window.addEventListener('load', () => void navigator.serviceWorker.register('./sw.js'));
}
renderPage();
