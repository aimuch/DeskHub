// 布局与显示：编辑草稿 → 下发 → 等设备确认。保存成功只代表 Agent 已记录，不代表设备已应用。
import { $, h } from '../lib/dom.js';
import { errorText, PRESETS, ROTATIONS, THEMES } from '../lib/copy.js';
import { clone } from '../lib/format.js';
import { activeDevices, canManage, deviceActions, getDevice, state } from '../lib/state.js';
import { badge, btn, clearMessage, configBadge, confirmAction, deviceName, notice, pageHead, panel, body, revisionText, showMessage } from '../lib/ui.js';
import { deviceSelector } from './common.js';

const MAX_CARDS = 4;

const defaults = () => ({
  schema_version: 1,
  display: { brightness: 50, rotation: 0, clock_s: 60, dim_s: 120, off_s: 300, night_enabled: true, night_start: 1320, night_end: 420, night_cap: 15, protection: true },
  layout: { preset: 'desk', theme: 'mint', action_ids: [], show_seconds: false, show_cpu: true },
});

function ensureDraft(device) {
  if (state.draft) return;
  const source = device?.desired_configuration || device?.reported_configuration;
  const base = defaults();
  state.draft = clone(source || base);
  state.draft.display = { ...base.display, ...state.draft.display };
  state.draft.layout = { ...base.layout, ...state.draft.layout };
  state.draftRevision = device?.desired_revision ?? 0;
}

function markDirty() {
  state.dirty = true;
  const status = $('draft-status');
  if (status) { status.textContent = '有未保存的修改'; status.className = 'dirty'; }
  const save = $('save-configuration');
  if (save) save.disabled = !canManage(getDevice());
  renderPreview();
}

// ---------- 表单控件 ----------
function switchRow(id, label, note, checked, onChange) {
  const input = h('input', { type: 'checkbox', id, checked });
  input.addEventListener('change', () => { onChange(input.checked); markDirty(); });
  return h('div', { class: 'switch-row' },
    h('div', { class: 'switch-copy' }, h('label', { for: id }, label), h('small', {}, note)),
    h('span', { class: 'switch' }, input, h('span')));
}

function numberField(id, label, value, min, max, onChange) {
  const input = h('input', { type: 'number', id, value: String(value), min, max, step: 1 });
  input.addEventListener('input', () => { onChange(input.value === '' ? NaN : Number(input.value)); markDirty(); });
  return h('div', { class: 'field' }, h('label', { for: id }, label), input);
}

const pad = number => String(number).padStart(2, '0');
function timeField(id, label, minutes, onChange) {
  const input = h('input', { type: 'time', id, value: `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}` });
  input.addEventListener('input', () => {
    const [hours, mins] = input.value.split(':').map(Number);
    onChange(hours * 60 + mins);
    markDirty();
  });
  return h('div', { class: 'field' }, h('label', { for: id }, label), input);
}

/** 单选按钮组：点击后只更新 aria-pressed，不重绘整页，避免丢失焦点。 */
function optionGroup(label, items, current, onPick, attribute) {
  const group = h('div', { class: 'option-grid', role: 'group', 'aria-label': label });
  const buttons = items.map(item => h('button', {
    type: 'button', class: 'option', dataset: { [attribute]: item.id }, 'aria-pressed': String(item.id === current),
    onclick: () => {
      onPick(item.id);
      buttons.forEach(button => button.setAttribute('aria-pressed', String(button.dataset[attribute] === item.id)));
      markDirty();
    },
  }, item.swatch ? h('span', { class: 'swatch', 'aria-hidden': 'true' }, h('i'), h('i'), h('i')) : null, h('strong', {}, item.name), item.description ? h('small', {}, item.description) : null));
  group.append(...buttons);
  return group;
}

// ---------- 编辑区 ----------
function appearanceSection() {
  const { display, layout } = state.draft;
  const rotation = h('select', { id: 'screen-rotation', 'aria-label': '屏幕方向', value: String(display.rotation) },
    ROTATIONS.map(degrees => h('option', { value: String(degrees) }, `${degrees}°`)));
  rotation.addEventListener('change', () => { display.rotation = Number(rotation.value); markDirty(); });
  const rotate = btn('旋转 90°', 'small', () => {
    display.rotation = ROTATIONS[(ROTATIONS.indexOf(display.rotation) + 1) % ROTATIONS.length];
    rotation.value = String(display.rotation);
    markDirty();
  }, { id: 'rotate-screen' });
  const box = panel('主题与方向');
  box.append(body(
    optionGroup('设备主题', THEMES.map(theme => ({ ...theme, swatch: true })), layout.theme, id => { layout.theme = id; }, 'theme'),
    h('div', { class: 'row' }, h('label', { for: 'screen-rotation', class: 'muted' }, '屏幕方向'), rotation, rotate),
    h('p', { class: 'small muted' }, '每次旋转 90°；设备应用后，显示与触摸方向一起改变。主题只改变配色，夜间亮度与屏幕保护仍然生效。')));
  return box;
}

function presetSection() {
  const { layout } = state.draft;
  const box = panel('主页预设');
  box.append(body(
    optionGroup('主页预设', PRESETS.map(([id, name]) => ({ id, name })), layout.preset, id => { layout.preset = id; }, 'preset'),
    h('p', { class: 'small muted' }, '预设使用固定的安全布局，保留主页与恢复入口。'),
    h('div', {},
      switchRow('show-seconds', '显示秒数', '使用设备当前有效的时间', layout.show_seconds, value => { layout.show_seconds = value; }),
      switchRow('show-cpu', '显示 CPU 摘要', '只有指标有效时才显示数值', layout.show_cpu, value => { layout.show_cpu = value; }))));
  return box;
}

function actionSection() {
  const box = panel('动作卡片', `最多 ${MAX_CARDS} 个；留空则使用设备默认顺序`);
  const area = h('div', { id: 'action-order-editor', class: 'action-order' });
  box.append(body(area));
  fillActionOrder(area);
  return box;
}

function fillActionOrder(area) {
  const available = deviceActions(getDevice());
  const chosen = state.draft.layout.action_ids;
  const rerender = () => { markDirty(); fillActionOrder(area); };
  area.replaceChildren();
  if (!chosen.length) {
    area.append(h('p', { class: 'muted' }, available.length
      ? '使用默认动作顺序。可以从下方选择动作，自定义最多 4 张卡片。'
      : '该设备暂无获准动作。请先在本机绑定显示器与动作，再给设备授权对应目标。'));
  }
  chosen.forEach((id, index) => {
    const label = available.find(item => item.action_id === id)?.label || id;
    const move = (text, aria, disabled, delta) => btn(text, 'small ghost', () => {
      [chosen[index], chosen[index + delta]] = [chosen[index + delta], chosen[index]];
      rerender();
    }, { 'aria-label': `${aria} ${label}`, disabled });
    area.append(h('div', { class: 'action-row' },
      h('span', { class: 'idx' }, pad(index + 1)), h('span', { class: 'label' }, label),
      move('↑', '向前移动', index === 0, -1), move('↓', '向后移动', index === chosen.length - 1, 1),
      btn('移除', 'small ghost', () => { chosen.splice(index, 1); rerender(); }, { 'aria-label': `移除 ${label}` })));
  });
  const options = available.filter(item => !chosen.includes(item.action_id));
  if (options.length && chosen.length < MAX_CARDS) {
    const select = h('select', { id: 'add-action', 'aria-label': '选择要添加的动作' }, options.map(item => h('option', { value: item.action_id }, item.label || item.action_id)));
    area.append(h('div', { class: 'row' }, select, btn('添加', 'small', () => { if (select.value) { chosen.push(select.value); rerender(); } }, { id: 'add-action-button' })));
  }
}

function displaySection() {
  const { display } = state.draft;
  const slider = h('input', { type: 'range', id: 'brightness', min: 3, max: 100, value: String(display.brightness), 'aria-label': '用户亮度' });
  const output = h('output', { for: 'brightness' }, `${display.brightness}%`);
  slider.addEventListener('input', () => { display.brightness = Number(slider.value); output.textContent = `${slider.value}%`; markDirty(); });

  const off = numberField('off-seconds', '息屏时间（秒）', display.off_s, 0, 86400, value => {
    display.off_s = value;
    $('always-on').checked = value === 0;
  });
  const alwaysOn = switchRow('always-on', '屏幕常亮', '一直保持当前页面与亮度，不进入时钟、不调暗、不息屏；夜间亮度上限与电源保护仍然生效', display.off_s === 0, value => {
    display.off_s = value ? 0 : Math.max(300, display.dim_s);
    $('off-seconds').value = String(display.off_s);
  });

  const box = panel('显示策略');
  box.append(body(
    h('div', { class: 'field' }, h('label', { for: 'brightness' }, '用户亮度'), h('div', { class: 'row' }, slider, output), h('span', { class: 'field-hint' }, '实际亮度仍受夜间与电源策略限制')),
    alwaysOn,
    h('div', { class: 'form-grid' },
      numberField('clock-seconds', '进入时钟（秒）', display.clock_s, 5, 86400, value => { display.clock_s = value; }),
      numberField('dim-seconds', '降亮时间（秒）', display.dim_s, 5, 86400, value => { display.dim_s = value; }),
      off),
    h('p', { class: 'small muted' }, '息屏时间 0 表示常亮，此时时钟与降亮时间不生效。关闭常亮时需满足：时钟 ≤ 降亮 ≤ 息屏。'),
    switchRow('protection', '屏幕保护', '低亮与待机微移，减少静态高亮负担', display.protection, value => { display.protection = value; })));
  return box;
}

function nightSection() {
  const { display } = state.draft;
  const box = panel('夜间模式');
  box.append(body(
    switchRow('night-enabled', '夜间亮度上限', '按设备本地时间执行，支持跨午夜', display.night_enabled, value => { display.night_enabled = value; }),
    h('div', { class: 'form-grid' },
      timeField('night-start', '开始时间', display.night_start, value => { display.night_start = value; }),
      timeField('night-end', '结束时间', display.night_end, value => { display.night_end = value; }),
      numberField('night-cap', '最高亮度（%）', display.night_cap, 3, 100, value => { display.night_cap = value; }))));
  return box;
}

// ---------- 校验与保存 ----------
export function validateConfiguration(config) {
  const { display, layout } = config;
  if (!THEMES.some(theme => theme.id === layout.theme)) return '请选择有效的设备主题。';
  if (!ROTATIONS.includes(display.rotation)) return '屏幕方向应为 0°、90°、180° 或 270°。';
  for (const key of ['brightness', 'clock_s', 'dim_s', 'off_s', 'night_start', 'night_end', 'night_cap']) {
    if (!Number.isInteger(display[key])) return '请填写完整的整数设置值。';
  }
  if (display.brightness < 3 || display.brightness > 100 || display.night_cap < 3 || display.night_cap > 100) return '亮度应在 3–100% 之间。';
  const badOff = display.off_s !== 0 && (display.off_s < display.dim_s || display.off_s > 86400);
  if (display.clock_s < 5 || display.clock_s > 86400 || display.dim_s < display.clock_s || display.dim_s > 86400 || badOff) {
    return '时间需满足：5 秒 ≤ 时钟 ≤ 降亮 ≤ 息屏 ≤ 86400 秒；息屏 0 表示永不。';
  }
  if (display.night_start < 0 || display.night_start > 1439 || display.night_end < 0 || display.night_end > 1439) return '请填写有效的夜间起止时间。';
  return '';
}

async function save(ctx) {
  if (!state.draft || state.busy) return;
  const invalid = validateConfiguration(state.draft);
  if (invalid) { showMessage(invalid, 'error'); return; }
  const device = getDevice();
  if (!canManage(device)) { showMessage('请先选择具备远程管理能力且未被吊销的设备。', 'warn'); return; }
  if (state.draft.display.off_s === 0 && !await confirmAction('设为永不自动息屏？', 'AMOLED 长时间显示静态内容会加速老化。夜间亮度限制和屏幕保护仍会生效。', '保存永不息屏')) return;
  state.busy = true;
  $('save-configuration').disabled = true;
  try {
    state.detail = await ctx.api.request(`/devices/${encodeURIComponent(device.device_id)}/configuration`, {
      method: 'PUT', json: { expected_revision: state.draftRevision, configuration: state.draft },
    });
    state.dirty = false;
    state.draft = null;
    showMessage('配置已保存在 Agent，等待设备验证并持久化。设备确认后会显示“设备已应用”。');
    ctx.render();
  } catch (error) {
    showMessage(errorText(error), 'error', error.code);
    if (error.status === 409) {
      $('global-message').append(btn('重新载入设备配置', 'small', async () => {
        state.draft = null; state.dirty = false;
        await ctx.refresh();
        clearMessage();
        ctx.render();
      }));
    }
  } finally {
    state.busy = false;
    const button = $('save-configuration');
    if (button) button.disabled = !canManage(getDevice());
  }
}

// ---------- 预览 ----------
const themeName = id => (THEMES.find(theme => theme.id === id) || THEMES[0]).name;

export function renderPreview() {
  const screen = $('layout-preview');
  if (!screen || !state.draft) return;
  const { layout, display } = state.draft;
  screen.dataset.theme = layout.theme;
  screen.dataset.preset = layout.preset;
  screen.dataset.rotation = String(display.rotation);
  screen.setAttribute('aria-label', `${themeName(layout.theme)}，屏幕方向 ${display.rotation} 度的布局示意`);
  const caption = $('preview-appearance');
  if (caption) caption.textContent = `${themeName(layout.theme)} · ${display.rotation}°`;

  const available = deviceActions(getDevice());
  const chosen = layout.action_ids.map(id => available.find(item => item.action_id === id)).filter(Boolean);
  const shown = (chosen.length ? chosen : available).slice(0, layout.preset === 'focus' ? MAX_CARDS : 2);
  screen.replaceChildren(h('div', { class: 'preview-screen' }, h('div', { class: 'preview-surface' },
    h('div', { class: 'pv-status' }, h('span', {}, 'Wi-Fi'), h('span', {}, '电量')),
    h('div', { class: 'pv-clock' }),
    h('div', { class: 'pv-date' }),
    layout.show_cpu ? h('div', { class: 'pv-metric' }, 'CPU · 连接后显示实际数据') : null,
    h('div', { class: 'pv-cards' }, shown.length ? shown.map((item, index) => h('div', { class: `pv-card${index === 0 ? ' on' : ''}` }, item.label)) : h('div', { class: 'pv-empty' }, '暂无获准动作')),
    h('div', { class: 'pv-nav' }, ['总览', '动作', '电脑', '设置'].map(text => h('span', {}, text))))));
  tickPreview();
}

/** 预览里的时间来自当前浏览器，只是示意。 */
export function tickPreview() {
  const clock = document.querySelector('#layout-preview .pv-clock');
  if (!clock || !state.draft) return;
  const now = new Date();
  clock.textContent = now.toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit', ...(state.draft.layout.show_seconds ? { second: '2-digit' } : {}) });
  const date = document.querySelector('#layout-preview .pv-date');
  if (date) date.textContent = now.toLocaleDateString('zh-CN', { month: 'long', day: 'numeric', weekday: 'long' });
}

export function renderLiveStatus() {
  const target = $('live-configuration-status');
  const device = getDevice();
  if (!target) return;
  target.replaceChildren(device ? h('span', { class: 'row' }, configBadge(device), h('span', { class: 'small muted' }, revisionText(device))) : badge('仅本地预览'));
}

export function renderLayout(container, ctx) {
  const hasDevices = activeDevices().length > 0;
  const device = hasDevices ? getDevice() : null;
  ensureDraft(device);

  const page = h('div', { class: 'page' }, pageHead('布局与显示', '选择主题、预设与屏幕方向，预览后再下发给设备。', ...ctx.tools()));
  if (hasDevices) page.append(deviceSelector(ctx));
  else page.append(notice('离线预览，尚未选择设备', '可以在本机切换主题、预设和屏幕方向，查看 480 × 480 的布局示意。草稿只保留在这个页面的内存中，不会保存或下发。'));

  const warning = !device ? '离线预览不会下发配置。连接并选择实际设备后，才能保存到设备。'
    : !canManage(device) ? '设备尚未上报远程管理能力。需先通过 USB 安装支持管理台的固件。' : '';

  const saveButton = btn('下发配置', 'primary', () => save(ctx), { id: 'save-configuration', disabled: !canManage(device) });
  const resetButton = btn('恢复草稿', 'ghost', () => { state.draft = null; state.dirty = false; clearMessage(); ctx.render(); });
  const bar = h('div', { class: 'savebar' },
    h('div', { class: 'copy' }, h('strong', {}, '配置草稿'), h('p', { id: 'draft-status' }, state.dirty ? '有未保存的修改' : '尚未修改'), h('p', { id: 'live-configuration-status' })),
    h('div', { class: 'row spacer' }, resetButton, saveButton));

  const editor = h('div', { class: 'layout-editor' },
    appearanceSection(), presetSection(), actionSection(), displaySection(), nightSection(),
    warning ? notice('暂时不能下发', warning, 'warn') : null, bar);

  const preview = h('aside', { class: 'layout-preview', 'aria-label': '布局预览' },
    h('div', { class: 'row' }, h('strong', {}, '布局预览'), h('span', { class: 'small muted spacer' }, '480 × 480')),
    h('div', { id: 'layout-preview', class: 'device-preview', role: 'img' }),
    h('div', { id: 'preview-appearance', class: 'small', 'aria-live': 'polite' }),
    h('p', { class: 'small muted' }, '这是布局示意，时间来自当前浏览器。设备确认之前，不代表实际页面已生效。'),
    h('div', { class: 'row small muted' }, badge(device ? deviceName(device) : '离线预览')));

  page.append(h('div', { class: 'layout-workspace' }, editor, preview));
  container.append(page);
  renderPreview();
  renderLiveStatus();
}
