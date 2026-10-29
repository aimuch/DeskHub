// 命令记录：每条命令的结果与证据。没有读回证据的不会显示为“已确认”。
import { h } from '../lib/dom.js';
import { shortTime } from '../lib/format.js';
import { state } from '../lib/state.js';
import { commandBadge, commandNote, empty, notice, pageHead, panel, table } from '../lib/ui.js';

const FILTERS = [['all', '全部', () => true], ['succeeded', '已确认', item => item.state === 'succeeded'],
  ['unconfirmed', '未确认', item => item.state === 'unconfirmed'], ['failed', '失败', item => item.state === 'failed']];
let filter = 'all';
let picked = '';

function detail(command) {
  if (!command) return h('p', { class: 'muted' }, '点一条命令，看它的细节。');
  const fact = (k, v) => h('div', { class: 'kv' }, h('span', {}, k), h('span', { class: 'mono' }, v ?? '无'));
  return h('div', { class: 'stack' },
    h('h2', {}, command.action_id),
    h('div', { class: 'small muted' }, `${shortTime(command.accepted_at)} · 来自 ${command.device_id}`),
    h('div', {}, commandBadge(command.state)),
    h('div', {},
      fact('命令编号', command.command_id),
      fact('目标', `${command.target_id} → ${command.requested_input}`),
      fact('读回输入源', command.observed_input || '没有读回'),
      fact('完成时间', command.completed_at ? shortTime(command.completed_at) : null),
      fact('原因码', command.reason_code || command.error_code)),
    commandNote(command) ? h('p', { class: 'small' }, `原因：${commandNote(command)}`) : null,
    command.state === 'unconfirmed' ? h('p', { class: 'small muted' }, '如果需要，请在设备上再点一次，或用命令行重试。Agent 不会自动重试。') : null);
}

export function renderCommands(container, ctx) {
  const page = h('div', { class: 'page' }, pageHead('命令记录', '“未确认”表示命令可能已经执行，但没有读回证据。Agent 不会自动重试。记录按 Agent 配置的保留期自动清理。', ...ctx.tools()));
  container.append(page);
  const data = state.commands;
  if (!data) {
    page.append(empty('正在读取', '正在向 Agent 读取命令记录。'));
    return;
  }
  const rerender = () => ctx.render();
  page.append(h('div', { class: 'row', role: 'group', 'aria-label': '筛选' }, FILTERS.map(([id, text]) =>
    h('button', { type: 'button', class: 'choice', dataset: { filter: id }, 'aria-pressed': String(filter === id), onclick: () => { filter = id; rerender(); } }, text))));
  const keep = FILTERS.find(([id]) => id === filter)[2];
  const shown = data.commands.filter(keep);
  const box = panel('最近命令', `${shown.length} / ${data.commands.length} 条`);
  if (!shown.length) {
    box.append(empty(data.commands.length ? '这个筛选下没有命令' : '还没有命令记录', data.commands.length ? '换一个筛选看看。' : '触屏或 deskhubctl 执行显示器动作后，结果会出现在这里。'));
  } else {
    box.append(table(['时间', '动作', '目标 → 输入', '结果', ''], shown.map(command => [
      h('span', { class: 'mono muted' }, shortTime(command.accepted_at)),
      h('div', {}, command.action_id, h('span', { class: 'cell-sub' }, `来自 ${command.device_id}`)),
      h('span', { class: 'mono small' }, `${command.target_id} → ${command.requested_input}`),
      h('div', {}, commandBadge(command.state), h('span', { class: 'cell-sub' }, commandNote(command))),
      h('button', { type: 'button', class: 'btn small ghost', dataset: { command: command.command_id }, onclick: () => { picked = command.command_id; rerender(); } }, '细节'),
    ])));
  }
  const aside = h('aside', { class: 'panel', 'aria-label': '命令详情' }, h('div', { class: 'panel-body' }, detail(data.commands.find(item => item.command_id === picked))));
  page.append(h('div', { class: 'cols' }, h('div', { class: 'grow' }, box), h('div', { class: 'side' }, aside)));
  if (data.truncated) page.append(notice('只显示了部分记录', '响应大小有上限，较早的记录未列出。'));
}
