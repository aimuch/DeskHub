// 概览：状态环、四张色卡、统计、设备、这台电脑、最近命令。没有数据的写“未上报”，不画 0。
import { h } from '../lib/dom.js';
import { backendReason, CONFIG_STATES, METRIC_REASONS } from '../lib/copy.js';
import { ago, describe, duration, rate, shortTime } from '../lib/format.js';
import { activeDevices, getDevice, hasActiveUpdate, isOnline, state } from '../lib/state.js';
import {
  btn, commandBadge, commandNote, empty, notice, onlineBadge, pageHead, panel, body, stat, table,
} from '../lib/ui.js';
import { powerLine, provisioningSteps, statusRing } from './common.js';

function metricValue(metric) {
  if (metric.value === null || metric.value === undefined) return null;
  if (metric.unit === 'percent') return `${Math.round(metric.value)}%`;
  if (metric.unit === 'byte_per_second') return rate(metric.value);
  if (metric.unit === 'duration_seconds') return duration(metric.value);
  return String(metric.value);
}

/** 过期的指标淡化并标注；没有值的指标写明原因，不显示 0。 */
function metricRow(metric) {
  const text = metricValue(metric);
  const stale = metric.quality === 'stale';
  const reason = METRIC_REASONS[metric.reason_code] || metric.reason_code || '暂无数据';
  const head = h('div', { class: 'head' }, h('span', { class: 'muted' }, metric.label), h('span', { class: 'mono' }, text === null ? reason : stale ? `${text}（已过期）` : text));
  const bar = text !== null && metric.unit === 'percent' ? h('progress', { class: 'bar', max: 100, value: metric.value, 'aria-label': `${metric.label} ${text}` }) : null;
  return h('div', { class: `meter-row${stale ? ' stale' : ''}` }, head, bar);
}

function hostPanel() {
  const box = panel('这台电脑', 'Agent 所在主机的真实指标');
  const host = state.host;
  if (!host) {
    box.append(body(h('p', { class: 'muted' }, '指标尚未读取。')));
    return box;
  }
  const order = ['cpu.utilization', 'memory.utilization', 'network.receive_rate', 'network.send_rate', 'system.uptime', 'cpu.temperature'];
  const metrics = order.map(key => host.metrics.find(metric => metric.key === key)).filter(Boolean);
  box.append(body(metrics.map(metricRow), h('p', { class: 'small muted' }, '不会用其他传感器顶替 CPU 温度。')));
  return box;
}

function ringState(device, backendReady) {
  return {
    wifi: isOnline(device),
    agent: backendReady,
    power: Boolean(device) && device.power_state === 'fresh' && !device.power?.fault,
    time: device?.configuration_state === 'applied',
  };
}

function tile(kind, label, value, sub) {
  return h('div', { class: 'tile', dataset: { k: kind } }, h('div', { class: 'k' }, label), h('div', { class: 'v' }, value), h('div', { class: 's' }, sub));
}

function tiles(device, overview, backendReady) {
  const listen = overview.listen;
  const config = device ? (CONFIG_STATES[device.configuration_state] || ['状态未知'])[0] : '未上报';
  const configSub = !device ? '没有设备' : device.desired_revision ? `revision ${device.desired_revision} · 已确认 ${device.applied_revision ?? 0}` : '网页没有下发过配置';
  const power = powerLine(device);
  return h('div', { class: 'tiles' },
    tile('wifi', '设备连接', device ? (isOnline(device) ? '在线' : '离线') : '未上报', device ? `最近上报：${ago(device.last_seen)}` : '还没有设备上报'),
    tile('agent', '电脑', backendReady ? '后端可用' : '后端不可用', listen ? `${listen.bind}:${listen.port} · ${listen.tls ? 'HTTPS' : '仅本机 HTTP'}` : '未读取'),
    tile('power', '电源', power.value, power.sub),
    tile('time', '配置', config, configSub));
}

function attention(devices, overview, backendReady) {
  const items = [];
  for (const device of devices) {
    if (device.configuration_state === 'failed') items.push([`${device.device_id} 没有应用配置：${describe(device.config_error)}。`, device.device_id]);
    else if (device.configuration_state === 'pending') items.push([`${device.device_id} 的配置在等待设备确认（revision ${device.desired_revision}）。网页保存成功，不等于设备已应用。`, device.device_id]);
    if (!isOnline(device)) items.push([device.last_seen ? `${device.device_id} 离线，最近上报：${ago(device.last_seen)}。` : `${device.device_id} 已配对，但还没有上报过。`, device.device_id]);
  }
  if (!backendReady) items.push([`显示器控制不可用：${backendReason(overview.backend?.reason_code) || '后端未就绪'}。`, null]);
  if (overview.firmware_trust_configured === false) items.push(['签名升级还没有配置公钥；普通设备管理不受影响。', null]);
  return items;
}

function devicePanel(device, overview, backendReady, ctx) {
  const box = panel('设备', null, device ? btn('全部设备', 'small ghost', () => ctx.go('devices')) : null);
  if (!device) {
    box.append(empty('还没有设备上报', '通过 USB 完成首次配置后，设备会在这里显示真实的运行状态。'), body(provisioningSteps()));
    return box;
  }
  const power = powerLine(device);
  const chips = [
    h('span', { class: 'pill', dataset: { k: isOnline(device) ? 'agent' : 'power' } }, isOnline(device) ? '在线' : '离线'),
    h('span', { class: 'pill', dataset: { k: device.configuration_state === 'applied' ? 'agent' : device.configuration_state === 'failed' ? 'danger' : 'power' } },
      device.configuration_state === 'applied' && !device.desired_revision ? '使用设备本地配置' : (CONFIG_STATES[device.configuration_state] || ['状态未知'])[0]),
    h('span', { class: 'pill' }, `固件 ${describe(device.firmware_version)}`),
  ];
  box.append(body(h('div', { class: 'row' },
    statusRing(ringState(device, backendReady), { mini: true }),
    h('div', { class: 'grow' }, h('strong', {}, device.device_id), h('div', { class: 'small muted' }, `${describe(device.board_model)} · ${describe(device.boot_id)}`), h('div', { class: 'row' }, chips)),
    h('div', { class: 'small muted' }, h('div', {}, power.value), h('div', {}, power.sub)))));
  return box;
}

function commandsPanel(ctx) {
  const commands = state.commands?.commands || [];
  const box = panel('最近命令', null, btn('全部记录', 'small ghost', () => ctx.go('commands')));
  if (!commands.length) {
    box.append(empty('还没有命令记录', '触屏或 deskhubctl 执行显示器动作后，结果会出现在这里。'));
    return box;
  }
  box.append(table(['时间', '动作', '目标', '结果'], commands.slice(0, 5).map(command => [
    h('span', { class: 'mono muted' }, shortTime(command.accepted_at)),
    command.action_id,
    h('span', { class: 'mono small' }, command.target_id),
    h('div', {}, commandBadge(command.state), h('span', { class: 'cell-sub' }, commandNote(command))),
  ])));
  return box;
}

export function renderOverview(container, ctx) {
  const overview = state.overview || {};
  const devices = activeDevices();
  const device = getDevice() && !getDevice().revoked ? getDevice() : devices[0];
  const online = devices.filter(isOnline).length;
  const applied = devices.filter(item => item.configuration_state === 'applied').length;
  const updating = state.devices.filter(hasActiveUpdate).length;
  const backendReady = overview.backend?.availability === 'ready';
  const issues = attention(devices, overview, backendReady);
  const now = new Date();

  const page = h('div', { class: 'page' },
    pageHead('概览', `${online} 台设备在线 · ${issues.length} 项需要处理。只显示已确认的状态，没有上报的写“未上报”。`, ...ctx.tools()));

  if (issues.length) {
    const [text, deviceId] = issues[0];
    page.append(h('div', { class: 'attention', role: 'status' },
      h('span', {}, `需要处理：${text}${issues.length > 1 ? `（另有 ${issues.length - 1} 项）` : ''}`),
      deviceId ? btn('查看设备', 'small', () => ctx.go('devices', deviceId)) : null));
  }

  page.append(h('section', { class: 'hero', 'aria-label': '设备总览' },
    statusRing(ringState(device, backendReady), {
      center: h('div', { class: 'ring-center' },
        h('div', { class: 'clock' }, now.toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit' })),
        h('div', { class: 'date' }, now.toLocaleDateString('zh-CN', { month: 'long', day: 'numeric', weekday: 'long' }))),
    }),
    tiles(device, overview, backendReady)));

  page.append(h('div', { class: 'stats' },
    stat('在线设备', online, `/ ${devices.length}`, online ? '30 秒内收到设备上报' : '等待设备建立安全连接'),
    stat('配置同步', applied, `/ ${devices.length}`, '设备确认后才计入'),
    stat('动作', overview.action_count ?? 0, '个', backendReady ? '后端可用，兼容性按显示器确认' : backendReason(overview.backend?.reason_code) || '后端不可用'),
    stat('进行中的升级', updating, '项', '首启自检确认后才算完成')));

  page.append(h('div', { class: 'cols' }, h('div', { class: 'grow' }, devicePanel(device, overview, backendReady, ctx)), h('div', { class: 'side' }, hostPanel())), commandsPanel(ctx));
  if (!devices.length) page.append(notice('还没有设备', '设备配对前，状态环是暗的：暗表示“没有数据”，不是“有故障”。'));
  container.append(page);
}
