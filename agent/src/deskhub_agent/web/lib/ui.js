// 跨页面共用的界面组件。
import { $, append, h, icon } from './dom.js';
import { COMMAND_STATES, CONFIG_STATES, UPDATE_STAGES, UPDATE_STATES, reasonText } from './copy.js';
import { describe, shortTime } from './format.js';
import { isOnline } from './state.js';

export const badge = (text, kind = 'neutral') => h('span', { class: `badge ${kind}` }, text);

export function pageHead(title, subtitle, ...tools) {
  return h('header', { class: 'page-head' },
    h('div', {}, h('h1', {}, title), subtitle && h('p', {}, subtitle)),
    tools.length ? h('div', { class: 'tools' }, tools) : null);
}

export function panel(title, subtitle, action) {
  return h('section', { class: 'panel' },
    h('div', { class: 'panel-head' }, h('div', {}, h('h2', {}, title), subtitle && h('p', {}, subtitle)), action ? h('div', { class: 'spacer' }, action) : null));
}

export const body = (...children) => h('div', { class: 'panel-body stack' }, children);

export function notice(title, detail = '', kind = '') {
  return h('div', { class: `notice ${kind}`, role: kind === 'bad' ? 'alert' : null },
    icon(kind ? 'alert' : 'info'),
    h('div', {}, h('strong', {}, title), detail && h('p', {}, detail)));
}

export function empty(title, description, ...actions) {
  return h('div', { class: 'empty' }, h('h3', {}, title), h('p', {}, description), actions);
}

export function stat(label, value, unit, foot) {
  return h('div', { class: 'stat' },
    h('div', { class: 'stat-label' }, label),
    h('div', { class: 'stat-value' }, value, unit ? h('small', {}, ` ${unit}`) : null),
    h('div', { class: 'stat-foot' }, foot));
}

export function table(headers, rows) {
  return h('div', { class: 'table-wrap' }, h('table', {},
    h('thead', {}, h('tr', {}, headers.map(text => h('th', { scope: 'col' }, text)))),
    h('tbody', {}, rows.map(cells => h('tr', {}, cells.map(cell => h('td', {}, cell)))))));
}

export const fact = (label, value, mono = false) =>
  h('div', { class: 'fact' }, h('dt', {}, label), h('dd', { class: mono ? 'mono' : '' }, describe(value)));

export const btn = (text, kind, onclick, extra = {}) => h('button', { type: 'button', class: `btn ${kind || ''}`.trim(), onclick, ...extra }, text);

/** 命令行片段 + 复制按钮（只复制命令文本，不含凭据）。 */
export function snippet(text) {
  const copy = btn([icon('copy'), '复制'], 'small ghost', async event => {
    try {
      await navigator.clipboard.writeText(text);
      event.currentTarget.lastChild.textContent = '已复制';
    } catch {
      event.currentTarget.lastChild.textContent = '请手动选择复制';
    }
  });
  return h('div', { class: 'stack' }, h('pre', { class: 'snippet' }, text), h('div', {}, copy));
}

export const deviceName = device => device?.device_id || '未选择设备';

export function deviceIdentity(device) {
  return h('div', {}, h('div', {}, h('strong', {}, deviceName(device))), h('div', { class: 'small muted mono' }, `配对于 ${shortTime(device.paired_at)}`));
}

export const onlineBadge = device => (device.revoked ? badge('已吊销', 'bad') : isOnline(device) ? badge('在线', 'ok') : badge('离线'));

export function configBadge(device) {
  // 管理台从未下发过配置时，设备跑的是它自己的本地配置，不能说成“已应用”。
  if (device?.configuration_state === 'applied' && !device.desired_revision) return badge('使用设备本地配置');
  const [text, kind] = CONFIG_STATES[device?.configuration_state] || ['状态未知', 'neutral'];
  return badge(text, kind);
}

export const revisionText = device => (device?.desired_revision
  ? `目标 r${device.desired_revision} / 已确认 r${device.applied_revision ?? 0}`
  : '尚未从管理台下发过配置');

export function commandBadge(state) {
  const [text, kind] = COMMAND_STATES[state] || [state, 'neutral'];
  return badge(text, kind);
}

export function commandNote(command) {
  return reasonText(command.reason_code || command.error_code);
}

/** 设备上报的升级进度。成功只来自设备首启自检后的上报。 */
export function updateStatus(device, { goFirmware } = {}) {
  const job = device?.update;
  const box = h('div', { class: 'stack', id: 'update-status' });
  if (!job) {
    box.append(h('p', { class: 'muted' }, '暂无升级任务。选择已验证的固件包后，设备会再次检查签名、版本与供电条件。'));
    if (goFirmware) box.append(h('div', {}, btn('前往固件库', 'small', goFirmware)));
    return box;
  }
  const kind = job.state === 'succeeded' ? 'ok' : ['failed', 'rolled_back'].includes(job.state) ? 'bad' : job.state === 'unconfirmed' ? 'warn' : 'neutral';
  const percent = Number.isFinite(job.progress_percent) ? Math.min(100, Math.max(0, job.progress_percent)) : null;
  const reached = UPDATE_STAGES.findIndex(([key]) => key === job.state);
  // append() from dom.js skips null; the native Element.append would print "null".
  append(box, [
    h('div', { class: 'row' }, h('strong', {}, describe(job.version)), badge(UPDATE_STATES[job.state] || '状态未知', kind)),
    percent === null ? null : h('progress', { class: 'bar', max: 100, value: percent, 'aria-label': '设备上报的升级进度' }),
    h('div', { class: 'row small muted' }, h('span', {}, '设备上报进度'), h('span', { class: 'spacer' }, percent === null ? '未上报' : `${percent}%`)),
    h('div', { class: 'stages' }, UPDATE_STAGES.map(([, label], index) => h('span', { class: index === reached ? 'on' : '' }, label))),
    h('p', { class: 'small muted' }, {
      queued: '任务已保存，尚未确认设备开始升级。',
      rebooting: '设备正在完成首启自检；现在还不能认为升级成功。',
      pending_confirmation: '设备正在完成首启自检；现在还不能认为升级成功。',
      succeeded: '设备已上报新固件通过首启自检。',
      unconfirmed: '暂未取得最终确认。请检查设备与连接，不要盲目重复发起升级。',
    }[job.state] || ''),
    job.error_code ? h('p', { class: 'small' }, badge(job.error_code, 'bad')) : null,
    h('p', { class: 'small muted mono' }, `任务 ${job.job_id}`)]);
  return box;
}

// ---------- 全局反馈 ----------
export function showMessage(text, kind = 'success', code = '') {
  const element = $('global-message');
  element.className = `message ${kind === 'success' ? '' : kind}`.trim();
  // replaceChildren() would render a null argument as the text "null".
  element.replaceChildren(...[h('span', {}, text), code ? h('span', { class: 'code' }, code) : null].filter(Boolean));
  element.hidden = false;
}
export const clearMessage = () => { $('global-message').hidden = true; };

let toastTimer;
export function toast(text) {
  const element = $('toast');
  element.textContent = text;
  element.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { element.hidden = true; }, 3500);
}

export function confirmAction(title, bodyText, label = '确认', danger = false) {
  const dialog = $('confirm-dialog');
  $('confirm-title').textContent = title;
  $('confirm-body').textContent = bodyText;
  const submit = $('confirm-submit');
  submit.textContent = label;
  submit.className = `btn ${danger ? 'danger' : 'primary'}`;
  dialog.returnValue = '';
  dialog.showModal();
  return new Promise(resolve => dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), { once: true }));
}
