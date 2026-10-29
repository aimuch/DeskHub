// DeskHub 管理台入口：会话、登录与设置密码、导航、轮询。页面逻辑在 pages/，通用工具在 lib/。
import { $, h, icon } from './lib/dom.js';
import { ApiError, createApi } from './lib/api.js';
import { errorText } from './lib/copy.js';
import { clockTime } from './lib/format.js';
import * as prefs from './lib/prefs.js';
import { activeDevices, getDevice, resetSession, state } from './lib/state.js';
import { clearMessage, confirmAction, showMessage, updateStatus } from './lib/ui.js';
import { renderAccess } from './pages/access.js';
import { renderActions } from './pages/actions.js';
import { renderAppearance } from './pages/appearance.js';
import { renderCommands } from './pages/commands.js';
import { renderDevices } from './pages/devices.js';
import { renderFirmware } from './pages/firmware.js';
import { renderLayout, renderLiveStatus, tickPreview } from './pages/layout.js';
import { renderOverview } from './pages/overview.js';
import { renderPower } from './pages/power.js';

const PAGES = {
  overview: { title: '概览', icon: 'overview', render: renderOverview },
  devices: { title: '设备', icon: 'devices', render: renderDevices },
  actions: { title: '动作与显示器', icon: 'actions', render: renderActions },
  commands: { title: '命令记录', icon: 'commands', render: renderCommands },
  layout: { title: '布局与显示', icon: 'layout', render: renderLayout },
  power: { title: '电源', icon: 'power', render: renderPower },
  firmware: { title: '固件升级', icon: 'firmware', render: renderFirmware },
  access: { title: '访问管理', icon: 'access', render: renderAccess },
  appearance: { title: '外观', icon: 'appearance', render: renderAppearance },
};
const POLL_MS = 10000;
// 总览与设备列表始终要取（导航角标、在线状态）；其余视图只在对应页面取，
// 这样多开标签页也不会撞到每个会话每分钟的请求配额。
const PAGE_VIEWS = { overview: ['host', 'commands'], devices: ['packages'], actions: ['displays'], commands: ['commands'], firmware: ['packages'] };
const MIN_PASSWORD = 12;

prefs.apply(prefs.load());

const api = createApi({ getCsrf: () => state.csrf, onUnauthorized: () => disconnect('会话已失效，请重新登录。') });
let refreshing = false;
let refreshQueued = false;

// ---------- 会话与三个入口视图 ----------
const VIEWS = ['boot', 'login-view', 'setup-view', 'application'];
function show(view) {
  for (const id of VIEWS) $(id).hidden = id !== view;
}

function showError(id, text) {
  const box = $(id);
  box.hidden = !text;
  box.textContent = text || '';
}

function pickTab(name) {
  const token = name === 'token';
  $('tab-password').setAttribute('aria-selected', String(!token));
  $('tab-token').setAttribute('aria-selected', String(token));
  $('password-form').hidden = token;
  $('token-form').hidden = !token;
  showError('password-error', '');
  showError('token-error', '');
  $(token ? 'login-token' : 'login-password').focus();
}

async function disconnect(message = '') {
  resetSession();
  show('login-view');
  $('login-password').value = '';
  $('login-token').value = '';
  // 还没设置过密码时，默认停在“口令（首次）”。
  let passwordSet = true;
  try { passwordSet = (await api.request('/auth')).password_set; } catch { /* 取不到就按已设置处理，不误导用户 */ }
  pickTab(passwordSet ? 'password' : 'token');
  showError(passwordSet ? 'password-error' : 'token-error', message);
}

function connected(session) {
  state.session = session;
  state.csrf = session.csrf_token;
  show('application');
}

function needsPassword(session) {
  state.session = session;
  state.csrf = session.csrf_token;
  show('setup-view');
  $('setup-password').value = '';
  $('setup-confirm').value = '';
  showError('setup-error', '');
  updateChecks();
  $('setup-password').focus();
}

// ---------- 数据 ----------
async function allDevices() {
  const devices = [];
  let offset = 0;
  for (let pages = 0; pages < 128; pages++) {
    const data = await api.request(`/devices?offset=${offset}`);
    devices.push(...(data.devices || []));
    if (data.next_offset === null || data.next_offset === undefined) return devices;
    if (!Number.isInteger(data.next_offset) || data.next_offset <= offset) throw new ApiError('INVALID_RESPONSE', 0);
    offset = data.next_offset;
  }
  throw new ApiError('INVALID_RESPONSE', 0);
}

// 次要视图失败时保留上一次的数据，不让它们拖垮主流程。401 由 api 层统一处理。
const optional = (path, previous) => api.request(path).catch(() => previous);

async function refresh(manual = false) {
  if (!state.session || state.session.setup_required) return;
  if (refreshing) { refreshQueued = true; return; } // 切页时进行中的刷新可能没带上新页面的数据
  refreshing = true;
  setRefreshDisabled(true);
  try {
    const views = new Set(PAGE_VIEWS[state.page] || []);
    const firstFirmwareLoad = views.has('packages') && state.packages === null;
    const view = (name, path, previous) => (views.has(name) ? optional(path, previous) : previous);
    const [overview, devices] = await Promise.all([api.request('/overview'), allDevices()]);
    const [firmware, host, displays, commands] = await Promise.all([
      views.has('packages') ? api.request('/firmware') : null,
      view('host', '/host', state.host), view('displays', '/displays', state.displays), view('commands', '/commands?limit=30', state.commands)]);
    Object.assign(state, { overview, devices, packages: firmware ? firmware.packages || [] : state.packages, host, displays, commands });
    if (!state.devices.some(device => device.device_id === state.selectedId && !device.revoked)) {
      state.selectedId = activeDevices()[0]?.device_id || '';
      state.detail = null;
    }
    if (state.selectedId) state.detail = await api.request(`/devices/${encodeURIComponent(state.selectedId)}`);
    state.lastRefresh = new Date();
    updateChrome();
    const editing = document.activeElement?.matches('input,select,textarea');
    // 正在编辑或上传时不整页重绘，只更新状态区，避免丢失输入。
    if (firstFirmwareLoad || (!state.dirty && !editing && !state.busy && !['layout', 'firmware', 'access', 'appearance'].includes(state.page))) render();
    else updateLive();
    // 成功不弹提示：页头的“更新于”时间已经说明。
  } catch (error) {
    if (state.session) {
      setConnection(false);
      if (manual) showMessage(errorText(error), 'error', error.code);
    }
  } finally {
    refreshing = false;
    setRefreshDisabled(false);
    if (refreshQueued) { refreshQueued = false; refresh(); }
  }
}

function updateLive() {
  renderLiveStatus();
  const current = $('update-status');
  const device = getDevice();
  if (current && device) current.replaceWith(updateStatus(device));
}

function setRefreshDisabled(disabled) {
  const button = $('refresh');
  if (button) button.disabled = disabled;
}

function setConnection(ok) {
  const label = $('connection-label');
  label.textContent = ok ? 'Agent 已连接' : 'Agent 连接中断';
  label.className = `badge ${ok ? 'ok' : 'bad'}`;
}

function updateChrome() {
  $('sidebar-agent').textContent = state.overview?.agent_id || '本地 Agent';
  $('nav-device-count').textContent = String(activeDevices().length);
  setConnection(true);
  const time = $('refresh-time');
  if (time && state.lastRefresh) time.textContent = `更新于 ${clockTime(state.lastRefresh)}`;
}

// ---------- 导航与渲染 ----------
async function selectDevice(id) {
  if (id === state.selectedId) return true;
  if (state.dirty && !await confirmAction('切换设备？', '当前未保存的修改将被丢弃。', '切换设备')) return false;
  state.selectedId = id;
  state.detail = null;
  state.draft = null;
  state.dirty = false;
  try {
    state.detail = await api.request(`/devices/${encodeURIComponent(id)}`);
    clearMessage();
  } catch (error) {
    showMessage(errorText(error), 'error', error.code);
  }
  render();
  return true;
}

async function go(page, deviceId = '') {
  if (state.dirty && !await confirmAction('离开未保存的布局？', '当前修改还没有下发。离开会丢弃这份草稿。', '丢弃并离开')) return;
  state.dirty = false;
  state.draft = null;
  state.page = page;
  history.replaceState(null, '', `#${page}`);
  if (deviceId && deviceId !== state.selectedId) {
    state.selectedId = deviceId;
    try { state.detail = await api.request(`/devices/${encodeURIComponent(deviceId)}`); } catch (error) { showMessage(errorText(error), 'error', error.code); }
  }
  clearMessage();
  render();
  // 新页面需要的视图还没读过，或数据已经旧了才补取；快速切页不会放大请求量。
  const missing = (PAGE_VIEWS[page] || []).some(name => state[name] === null);
  if (missing || !state.lastRefresh || Date.now() - state.lastRefresh > 5000) refresh();
}

/** 每个页面头部右侧的“更新时间 + 刷新”。 */
function tools() {
  return [
    h('span', { id: 'refresh-time', class: 'small' }, state.lastRefresh ? `更新于 ${clockTime(state.lastRefresh)}` : '正在获取状态'),
    h('button', { id: 'refresh', type: 'button', class: 'icon-btn', 'aria-label': '刷新状态', title: '刷新状态', onclick: () => refresh(true) }, icon('refresh')),
  ];
}

const ctx = { api, go, refresh, render, selectDevice, tools, prefs, signOut: () => signOut() };

function render() {
  if (!state.session || state.session.setup_required) return;
  const page = PAGES[state.page] || PAGES.overview;
  document.title = `${page.title} · DeskHub 管理台`;
  document.querySelectorAll('.nav-item').forEach(item => {
    if (item.dataset.page === state.page) item.setAttribute('aria-current', 'page');
    else item.removeAttribute('aria-current');
  });
  const content = $('content');
  content.replaceChildren();
  if (!state.overview) {
    content.append(h('p', { class: 'muted', role: 'status' }, '正在读取 Agent 状态…'));
    return;
  }
  page.render(content, ctx);
}

function buildNavigation() {
  const nav = $('nav');
  for (const [key, page] of Object.entries(PAGES)) {
    nav.append(h('button', { type: 'button', class: 'nav-item', dataset: { page: key }, onclick: () => go(key).catch(error => showMessage(errorText(error), 'error', error.code)) },
      icon(page.icon), h('span', {}, page.title), key === 'devices' ? h('span', { id: 'nav-device-count', class: 'nav-count' }, '0') : null));
  }
}

// ---------- 登录 ----------
async function enter(session) {
  connected(session);
  render();
  await refresh();
  render();
}

async function passwordSignIn(event) {
  event.preventDefault();
  const submit = $('password-submit');
  submit.disabled = true;
  showError('password-error', '');
  try {
    const session = await api.request('/login', { method: 'POST', json: { password: $('login-password').value } });
    $('login-password').value = '';
    await enter(session);
  } catch (error) {
    $('login-password').value = '';
    showError('password-error', errorText(error));
    $('login-password').focus();
  } finally {
    submit.disabled = false;
  }
}

async function tokenSignIn(event) {
  event.preventDefault();
  const submit = $('token-submit');
  submit.disabled = true;
  showError('token-error', '');
  try {
    const session = await api.request('/session', { method: 'POST', json: { token: $('login-token').value.trim() } });
    $('login-token').value = '';
    if (session.setup_required) needsPassword(session);
    else await enter(session);
  } catch (error) {
    $('login-token').value = '';
    if (error.code === 'TOKEN_LOGIN_DISABLED') pickTab('password');
    showError(error.code === 'TOKEN_LOGIN_DISABLED' ? 'password-error' : 'token-error',
      error.status === 401 || (error.status === 403 && error.code !== 'TOKEN_LOGIN_DISABLED')
        ? '口令无效或没有管理权限。请使用本机生成的管理员口令。' : errorText(error));
  } finally {
    submit.disabled = false;
  }
}

// ---------- 设置密码 ----------
function checks() {
  const first = $('setup-password').value;
  const second = $('setup-confirm').value;
  return [
    [`至少 ${MIN_PASSWORD} 个字符`, [...first].length >= MIN_PASSWORD],
    ['两次输入一致', first !== '' && first === second],
    ['常见的弱密码、重复或连续的字符、和口令相同的密码会被拒绝（提交时检查）', null],
  ];
}

function updateChecks() {
  const list = checks();
  $('setup-checks').replaceChildren(...list.map(([text, pass]) => h('li', { class: pass ? 'pass' : '' }, text)));
  $('setup-submit').disabled = !list.slice(0, 2).every(([, pass]) => pass);
}

async function setPassword(event) {
  event.preventDefault();
  const submit = $('setup-submit');
  submit.disabled = true;
  showError('setup-error', '');
  try {
    const session = await api.request('/password', { method: 'PUT', json: { password: $('setup-password').value } });
    $('setup-password').value = '';
    $('setup-confirm').value = '';
    await enter(session);
  } catch (error) {
    showError('setup-error', errorText(error));
    updateChecks();
  }
}

async function signOut() {
  if (state.dirty && !await confirmAction('退出管理台？', '未保存的草稿会被丢弃。', '退出')) return;
  try {
    await api.request('/session', { method: 'DELETE' });
    await disconnect();
  } catch (error) {
    showMessage(errorText(error), 'error', error.code);
  }
}

function start() {
  buildNavigation();
  const initial = location.hash.slice(1);
  if (PAGES[initial]) state.page = initial;
  $('tab-password').addEventListener('click', () => pickTab('password'));
  $('tab-token').addEventListener('click', () => pickTab('token'));
  $('password-form').addEventListener('submit', passwordSignIn);
  $('token-form').addEventListener('submit', tokenSignIn);
  $('setup-form').addEventListener('submit', setPassword);
  $('setup-password').addEventListener('input', updateChecks);
  $('setup-confirm').addEventListener('input', updateChecks);
  $('logout').addEventListener('click', signOut);
  window.addEventListener('beforeunload', event => { if (state.dirty) { event.preventDefault(); event.returnValue = ''; } });
  document.addEventListener('visibilitychange', () => { if (!document.hidden && state.session) refresh(); });
  setInterval(() => { if (state.session && !document.hidden) refresh(); }, POLL_MS);
  setInterval(() => { if (state.page === 'layout' && state.session) tickPreview(); }, 1000);
  api.request('/session')
    .then(async session => { if (session.setup_required) needsPassword(session); else await enter(session); })
    .catch(() => disconnect());
}

start();
