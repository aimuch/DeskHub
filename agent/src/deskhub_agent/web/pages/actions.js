// 动作与显示器：只读视图。绑定与输入映射在本机配置文件里修改，网页不写入控制命令。
import { h } from '../lib/dom.js';
import { backendReason } from '../lib/copy.js';
import { state } from '../lib/state.js';
import { badge, body, empty, notice, pageHead, panel, table } from '../lib/ui.js';

const availability = capability => {
  if (!capability) return badge('未知');
  if (capability.availability === 'ready') return badge('可用', 'ok');
  return badge(backendReason(capability.reason_code) || '不可用', 'warn');
};
const verified = value => (value ? badge('已验证', 'ok') : badge('未验证', 'warn'));

function backendPanel(backend) {
  const box = panel('显示器后端');
  const ready = backend.availability === 'ready';
  box.append(body(
    h('div', { class: 'row' },
      badge(ready ? '可用' : backend.configured ? '故障' : '未配置', ready ? 'ok' : 'warn'),
      backend.version ? h('span', { class: 'small muted' }, `BetterDisplay ${backend.version}`) : null,
      backend.reason_code ? h('span', { class: 'chip spacer' }, backend.reason_code) : null),
    h('p', { class: 'small muted' }, ready
      ? '后端可用不代表每台显示器都能切回。请按显示器分别验证兼容性与切回路径。'
      : `${backendReason(backend.reason_code) || '后端不可用'}。不可用时，触屏与 deskhubctl 的显示器动作会返回“不可用”，不会产生假成功。`)));
  return box;
}

function displayCard(display) {
  const observed = display.observed || {};
  const current = observed.quality === 'valid' && observed.input_id ? observed.input_id : '未知（无法读回）';
  const card = h('article', { class: 'panel' },
    h('div', { class: 'panel-head' }, h('h2', {}, display.label), h('span', { class: 'chip' }, display.monitor_id)),
    body(
      h('dl', { class: 'dl' },
        h('dt', {}, '当前输入源'), h('dd', {}, observed.quality === 'stale' ? `${current}（已过期）` : current),
        h('dt', {}, '设置输入源'), h('dd', {}, availability(display.capabilities?.set_input)),
        h('dt', {}, '读回'), h('dd', {}, display.readback_enabled ? availability(display.capabilities?.read_input) : badge('未启用')),
        h('dt', {}, '兼容性验证'), h('dd', {}, verified(display.compatibility_verified)),
        h('dt', {}, '切回路径'), h('dd', {}, verified(display.return_path_verified))),
      h('div', { class: 'small muted' }, '输入映射'),
      h('div', { class: 'row' }, (display.input_ids || []).map(id => h('span', { class: 'chip' }, id)))));
  return card;
}

export function renderActions(container, ctx) {
  const data = state.displays;
  const page = h('div', { class: 'page' },
    pageHead('动作与显示器', '只读视图。显示器绑定和输入映射在本机配置文件中修改。', ...ctx.tools()));
  container.append(page);
  if (!data) {
    page.append(empty('正在读取', '正在向 Agent 读取显示器与动作配置。'));
    return;
  }
  page.append(backendPanel(data.backend));

  if (!data.displays.length) {
    page.append(notice('尚未绑定显示器', '在 Agent 配置文件中添加显示器的绑定与输入映射后，这里会显示它们。绑定标识（UUID、序列号）不会在网页中显示。'));
  } else {
    page.append(h('div', { class: 'stats' }, data.displays.map(displayCard)));
  }

  const actions = panel('已注册动作', '触屏、deskhubctl 和快捷键工具共用这些动作');
  if (!data.actions.length) actions.append(empty('还没有动作', '动作把一个目标显示器和一个输入映射关联起来，例如“切到 Mac”。'));
  else {
    actions.append(table(['名称', '动作 ID', '目标 → 输入', '快捷键', '状态'], data.actions.map(action => [
      h('strong', {}, action.label),
      h('span', { class: 'mono small' }, action.action_id),
      h('span', { class: 'mono small' }, `${action.target_id} → ${action.input_id}`),
      action.shortcut ? h('span', { class: 'chip' }, action.shortcut) : h('span', { class: 'muted' }, '未设置'),
      badge(action.enabled ? '已启用' : '已停用', action.enabled ? 'ok' : 'neutral'),
    ])));
  }
  page.append(actions);
}
