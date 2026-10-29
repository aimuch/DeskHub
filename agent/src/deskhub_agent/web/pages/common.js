// 页面间共用：设备选择器、接入指引、状态环、电源摘要。
import { h, svg } from '../lib/dom.js';
import { celsius, NOT_REPORTED, volts } from '../lib/format.js';
import { activeDevices, getDevice, state } from '../lib/state.js';
import { deviceName, onlineBadge, snippet } from '../lib/ui.js';

/** 只有一台设备时不显示下拉框，直接显示它的身份。 */
export function deviceSelector(ctx) {
  const devices = activeDevices();
  const current = getDevice();
  const select = h('select', {
    id: 'selected-device',
    'aria-label': '当前设备',
    value: state.selectedId,
    onchange: () => ctx.selectDevice(select.value).then(ok => { if (!ok) select.value = state.selectedId; }),
  }, devices.map(device => h('option', { value: device.device_id }, deviceName(device))));
  return h('div', { class: 'row' },
    h('label', { for: 'selected-device', class: 'muted' }, '当前设备'),
    devices.length > 1 ? select : h('strong', {}, deviceName(current)),
    current ? onlineBadge(current) : null);
}

export const PROVISION_COMMAND = '.venv/bin/python tools/provision.py --port <设备串口> --agent-config <Agent 配置文件> --ca <CA 证书> --url https://<Mac 的局域网地址>:8765';

/** 接入新设备的三步。Wi-Fi 密码只在本机交互输入，不放进命令行。 */
export function provisioningSteps() {
  return h('div', { class: 'stack' },
    h('ol', { class: 'steps' },
      h('li', {}, h('div', {}, h('h3', {}, '连接设备'), h('p', {}, '用 USB 接入 DeskHub，退出占用串口的程序。'))),
      h('li', {}, h('div', {}, h('h3', {}, '运行配对命令'), h('p', {}, '在运行 Agent 的电脑上运行下面的命令，写入 Wi-Fi、受信 CA 和这台设备独立的凭据。Wi-Fi 密码在提示中输入，不写在命令里。'))),
      h('li', {}, h('div', {}, h('h3', {}, '等待设备上报'), h('p', {}, '设备联网后会出现在这里；在它确认之前，不会显示“已应用”。')))),
    snippet(PROVISION_COMMAND));
}

const ARCS = {
  wifi: 'M121.6 11.0 A98 98 0 0 1 205.0 94.4',
  agent: 'M205.0 121.6 A98 98 0 0 1 121.6 205.0',
  power: 'M94.4 205.0 A98 98 0 0 1 11.0 121.6',
  time: 'M11.0 94.4 A98 98 0 0 1 94.4 11.0',
};
export const RING_NAMES = { wifi: '设备连接', agent: '电脑', power: '电源', time: '配置' };

/** 四段状态环。亮的段表示这一项正常；暗的段表示未上报、离线或有问题。 */
export function statusRing(lit, { mini = false, center = null } = {}) {
  const text = Object.keys(ARCS).map(key => `${RING_NAMES[key]}${lit[key] ? '正常' : '需要关注'}`).join('，');
  return h('div', { class: `ring${mini ? ' mini' : ''}`, role: 'img', 'aria-label': `状态环：${text}` },
    svg('svg', { viewBox: '0 0 216 216', 'aria-hidden': 'true' },
      svg('circle', { class: 'track', cx: 108, cy: 108, r: 98 }),
      Object.entries(ARCS).map(([key, d]) => svg('path', { class: `seg${lit[key] ? '' : ' off'}`, 'data-k': key, d }))),
    center);
}

const CHARGING = { charging: '充电中', full: '已充满', idle: '未充电', fault: '充电故障' };

/** 一行电源摘要。没有上报的写“未上报”，数据过期的写“数据已过期”，不显示旧数字。 */
export function powerLine(device) {
  if (!device || device.power_state === 'unreported') return { value: NOT_REPORTED, sub: '设备还没有上报电源读数', fresh: false };
  const power = device.power || {};
  if (device.power_state === 'stale') return { value: '数据已过期', sub: '设备已经超过 30 秒没有上报', fresh: false };
  const level = Number.isFinite(power.soc_percent) ? `${power.soc_percent}%` : power.battery === 'absent' ? '无电池' : '电量未知';
  const parts = [Number.isFinite(power.battery_mv) ? volts(power.battery_mv) : null,
    Number.isFinite(power.vbus_mv) ? `USB ${volts(power.vbus_mv)}` : null,
    Number.isFinite(power.chip_temp_c) ? celsius(power.chip_temp_c) : null].filter(Boolean);
  return { value: `${level} · ${CHARGING[power.charging] || '状态未知'}`, sub: parts.join(' · ') || '电压和温度未测量', fresh: true };
}
