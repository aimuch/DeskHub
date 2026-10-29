import { h } from '../lib/dom.js';
import { describe, bytes, duration, NOT_REPORTED, shortTime } from '../lib/format.js';
import { getDevice, activeDevices, state } from '../lib/state.js';
import {
  badge, body, btn, configBadge, deviceName, empty, fact, notice, onlineBadge, pageHead, panel, revisionText, updateStatus,
} from '../lib/ui.js';
import { deviceSelector, provisioningSteps } from './common.js';

const VBUS = { present: '已接入', absent: '未接入' };
const BATTERY = { present: '已安装', absent: '未安装' };
const CHARGING = { charging: '正在充电', full: '已充满', idle: '空闲', fault: '故障' };

export function renderDevices(container, ctx) {
  const page = h('div', { class: 'page' }, pageHead('设备', '查看真实运行状态，管理每一台桌面终端。', ...ctx.tools()));
  container.append(page);

  if (!activeDevices().length) {
    const box = panel('等待第一台设备');
    box.append(empty('还没有设备上报', '设备通过 USB 配对后，管理台会保留它的身份与上报记录。离线设备不会被显示为在线。'), body(provisioningSteps()));
    page.append(box);
    return;
  }

  page.append(deviceSelector(ctx));
  const device = getDevice();
  if (!device) return;
  const power = device.power || {};

  const summary = panel(deviceName(device), `启动标识 ${describe(device.boot_id)}`, onlineBadge(device));
  summary.append(body(h('dl', { class: 'facts' },
    fact('固件版本', device.firmware_version), fact('硬件型号', device.board_model), fact('最近上报', shortTime(device.last_seen)),
    fact('运行时间', duration(device.uptime_seconds)), fact('可用堆内存', bytes(device.free_heap_bytes)),
    fact('USB 电源', VBUS[power.vbus] || '状态未知'), fact('电池', BATTERY[power.battery] || '状态未知'),
    fact('充电状态', CHARGING[power.charging] || '状态未知'))));
  page.append(summary);

  const configuration = panel('配置状态', '目标配置与设备确认分别记录');
  const step = (title, note, mark) => h('li', { class: `step ${mark}` }, h('span', { class: 'dot', 'aria-hidden': 'true' }), h('div', {}, h('div', {}, title), h('div', { class: 'small muted' }, note)));
  const saved = device.desired_revision > 0;
  const applied = device.configuration_state === 'applied' && saved;
  const failed = device.configuration_state === 'failed';
  configuration.append(body(
    h('div', { class: 'row' }, configBadge(device), h('span', { class: 'small muted' }, revisionText(device))),
    saved ? h('ol', { class: 'steplist' },
      step(`Agent 已保存 revision ${device.desired_revision}`, '网页保存成功只代表这一步', 'done'),
      step(failed ? '设备没有应用' : '等待设备确认', failed ? describe(device.config_error) : '设备要先校验、保存，并重绘界面，才会上报确认', applied ? 'done' : failed ? 'bad' : 'now'),
      step('设备已应用', '设备上报的 revision 与完整配置一致后才显示', applied ? 'done' : 'todo')) : null,
    device.config_error && !saved ? notice('设备未应用这份配置', device.config_error, 'bad') : null,
    h('div', {}, btn('调整布局与显示', '', () => ctx.go('layout', device.device_id)))));

  const updates = panel('固件升级', '进度由设备上报');
  updates.append(body(updateStatus(device, { goFirmware: () => ctx.go('firmware', device.device_id) })));

  const capabilities = device.capabilities || {};
  const trust = panel('升级信任', '设备、Agent 和固件包的签名公钥指纹必须一致');
  const fingerprint = value => (value ? `${value.slice(0, 8)}…${value.slice(-4)}` : '未配置');
  const packageKeys = [...new Set((state.packages || []).map(item => item.key_fingerprint))];
  trust.append(body(
    h('div', { class: 'row' },
      badge(capabilities.management ? '远程配置可用' : '管理能力尚未上报', capabilities.management ? 'ok' : 'neutral'),
      badge(capabilities.signed_ota ? '签名 OTA 已启用' : '签名 OTA 不可用', capabilities.signed_ota ? 'ok' : 'neutral')),
    h('div', {},
      h('div', { class: 'kv' }, h('span', {}, 'Agent'), h('span', { class: 'mono' }, state.overview?.firmware_trust_configured ? '已配置公钥' : '未配置')),
      h('div', { class: 'kv' }, h('span', {}, '固件包'), h('span', { class: 'mono' }, state.packages === null ? NOT_REPORTED : packageKeys.length ? packageKeys.map(fingerprint).join('、') : '固件库是空的')),
      h('div', { class: 'kv' }, h('span', {}, '设备'), h('span', { class: 'mono' }, fingerprint(capabilities.update_key_fingerprint)))),
    h('p', { class: 'small muted' }, '接口存在不等于功能已验证。签名升级只在 Wi-Fi OTA 时用到；USB 刷机不需要。')));

  page.append(h('div', { class: 'cols' }, h('div', { class: 'grow' }, configuration), h('div', { class: 'side' }, updates)), trust);
}
