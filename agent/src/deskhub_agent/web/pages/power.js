// 电源：设备上报的电量、电压、温度和测量状态。只显示新鲜的数据；没有上报写“未上报”，过期写“数据已过期”。
import { h, svg } from '../lib/dom.js';
import { ago, celsius, describe, NOT_REPORTED, volts } from '../lib/format.js';
import { activeDevices, getDevice } from '../lib/state.js';
import { badge, body, empty, notice, pageHead, panel } from '../lib/ui.js';
import { deviceSelector, provisioningSteps } from './common.js';

const VBUS = { present: '已接入', absent: '未接入', unknown: '状态未知' };
const BATTERY = { present: '已安装', absent: '未安装', unknown: '状态未知' };
const CHARGING = { charging: '正在充电', full: '已充满', idle: '空闲', fault: '故障', unknown: '状态未知' };
const MEASUREMENT = {
  ok: ['读数正常', 'ok'],
  adc_enable_failed: ['打开 ADC 失败，没有电压和温度', 'bad'],
  adc_read_failed: ['读 ADC 失败，没有电压和温度', 'bad'],
  adc_out_of_range: ['读数超出合理范围，已丢弃', 'bad'],
  not_measured: ['没有测量', 'neutral'],
};

/** 270° 的仪表，缺口在下方，从 135° 顺时针。读数无效时只画轨道，不填 0。 */
function gauge(percent) {
  const point = degrees => [65 + 50 * Math.cos(degrees * Math.PI / 180), 65 + 50 * Math.sin(degrees * Math.PI / 180)].map(value => value.toFixed(2));
  const [x0, y0] = point(135);
  const [x1, y1] = point(405);
  const track = `M${x0} ${y0} A50 50 0 1 1 ${x1} ${y1}`;
  const valid = Number.isFinite(percent);
  const sweep = valid ? 270 * Math.min(100, Math.max(0, percent)) / 100 : 0;
  const [ex, ey] = point(135 + sweep);
  const parts = [svg('path', { class: 'track', d: track })];
  if (valid && sweep > 0) parts.push(svg('path', { class: 'fill', d: `M${x0} ${y0} A50 50 0 ${sweep > 180 ? 1 : 0} 1 ${ex} ${ey}` }));
  return h('div', { class: 'gauge', role: 'img', 'aria-label': valid ? `电量仪表 ${percent}%` : '电量仪表：未上报' },
    svg('svg', { viewBox: '0 0 130 130', 'aria-hidden': 'true' }, parts),
    h('div', { class: 'num' }, valid ? `${percent}%` : '—', h('small', {}, '电量')));
}

function reading(label, value, note) {
  return h('div', { class: 'reading' }, h('div', { class: 'k' }, label), h('div', { class: 'v' }, value), h('div', { class: 's' }, note));
}

export function renderPower(container, ctx) {
  const devices = activeDevices();
  const device = getDevice();
  const page = h('div', { class: 'page' }, pageHead('电源', '电量、电压、温度和充电保护。读数来自电源芯片 AXP2101。', ...ctx.tools()));
  container.append(page);
  if (!devices.length || !device) {
    const box = panel('等待第一台设备');
    box.append(empty('还没有设备上报', '设备配对并联网后，这里显示它上报的电源读数。'), body(provisioningSteps()));
    page.append(box);
    return;
  }
  page.append(deviceSelector(ctx));

  const power = device.power || {};
  const fresh = device.power_state === 'fresh';
  const stamp = device.power_state === "unreported" ? NOT_REPORTED : fresh ? `${ago(device.power_reported_at)}更新` : "数据已过期";
  const [measureText, measureKind] = MEASUREMENT[power.measurement] || ['状态未知', 'neutral'];

  page.append(
    notice('固件只写白名单里的电源芯片寄存器',
      '打开 ADC 用 0x30 的位 0、2、4；充电保护开启时才用 0x18 的位 1。每次写之前先认芯片型号，写完读回确认。其他寄存器一律不写。'),
    h('p', { class: 'small muted' }, `${device.device_id} · ${stamp}`));

  const live = panel('实时读数', fresh ? null : '设备没有新鲜的上报，不显示旧数字');
  const level = fresh && power.battery === 'present' ? power.soc_percent : null;
  live.append(body(h('div', { class: 'row' }, gauge(Number.isFinite(level) ? level : null), h('div', { class: 'readings' },
    reading('电量', Number.isFinite(level) ? `${level}%` : NOT_REPORTED, 'PMIC 的估算，没有对照过这颗电芯'),
    reading('电池电压', fresh ? volts(power.battery_mv) : NOT_REPORTED, '合理范围 2.5–4.6 V'),
    reading('USB 电压', fresh ? volts(power.vbus_mv) : NOT_REPORTED, '合理范围 3.0–6.5 V'),
    reading('芯片温度', fresh ? celsius(power.chip_temp_c) : NOT_REPORTED, '电源芯片本身，不是电池温度')))));

  const status = panel('测量状态');
  const row = (name, note, mark) => h('div', { class: 'channel' }, h('strong', {}, name), h('span', { class: 'note' }, note), mark);
  status.append(body(h('div', {},
    row('电池', '电池检测', badge(fresh ? BATTERY[power.battery] || '状态未知' : NOT_REPORTED, fresh && power.battery === 'present' ? 'ok' : 'neutral')),
    row('USB 电源', '5 V 输入', badge(fresh ? VBUS[power.vbus] || '状态未知' : NOT_REPORTED, fresh && power.vbus === 'present' ? 'ok' : 'neutral')),
    row('充电状态', '充电器状态', badge(fresh ? CHARGING[power.charging] || '状态未知' : NOT_REPORTED, fresh && power.charging === 'fault' ? 'bad' : 'neutral')),
    row('电源故障', '芯片报告的故障位', badge(fresh ? (power.fault ? '有故障' : '无') : NOT_REPORTED, fresh && power.fault ? 'bad' : 'neutral')),
    row('ADC 测量', '电池电压、USB 电压、芯片温度', badge(fresh ? measureText : NOT_REPORTED, fresh ? measureKind : 'neutral')),
    row('电池温度', '没有已确认的温度探头，TS 引脚不使用', badge('不可用')))));

  const protection = panel("充电保护", "默认关闭");
  protection.append(body(
    h('p', {}, '电量到上限就停止充电，降到回充点再充。开关在设备上：设置 → 电池与电源 → 保护，开启要二次确认。'),
    notice('网页暂时不能开关它', '设备的上报里还没有“充电保护是否开启”和“充电事件”，所以这里不显示它们，也不放一个不会生效的开关。', 'warn'),
    h('div', {}, h('div', { class: 'small muted' }, '保护措施（设备固件执行）'),
      h('ul', { class: 'plain' },
        h('li', {}, '电池电压不小于 4.30 V，或芯片温度不小于 65 °C：立刻停充'),
        h('li', {}, '连续 3 个有效样本才切换，两次切换至少隔 60 秒'),
        h('li', {}, '读不到电量：恢复正常充电，由芯片自己的终止机制兜底'),
        h('li', {}, '每次开关都读回确认，并记入设备日志'))),
    h('p', { class: 'small muted' }, `这一功能还没有在真机上验证。当前设备固件 ${describe(device.firmware_version)}。`)));

  page.append(h('div', { class: 'cols' }, h('div', { class: 'grow stack' }, live, status), h('div', { class: 'side' }, protection)));
}
