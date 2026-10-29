import { h } from '../lib/dom.js';
import { errorText } from '../lib/copy.js';
import { bytes, describe, shortTime } from '../lib/format.js';
import { activeDevices, getDevice, hasActiveUpdate, isOnline, state } from '../lib/state.js';
import {
  badge, body, btn, clearMessage, confirmAction, deviceName, empty, notice, pageHead, panel, showMessage, table, toast, updateStatus,
} from '../lib/ui.js';
import { deviceSelector } from './common.js';

const MAX_PACKAGE_BYTES = 7 * 1024 * 1024;

/** 返回不能升级的原因；空字符串表示可以发起。设备端和 Agent 还会再次校验。 */
function updateBlocker(device) {
  if (!device) return '请先选择设备。';
  if (!isOnline(device)) return '设备当前离线。等它重新上报后再升级。';
  if (device.power?.vbus !== 'present' || device.power?.fault) return '设备需要稳定的 USB 供电，且电源状态无故障。';
  if (!device.capabilities?.signed_ota) return '设备尚未启用签名 OTA。请先通过 USB 安装支持的固件并配置升级公钥。';
  if (hasActiveUpdate(device)) return '设备已有升级任务。请等当前任务结束。';
  return '';
}

async function install(item, ctx) {
  const device = getDevice();
  const blocker = updateBlocker(device);
  if (blocker) { showMessage(blocker, 'warn'); return; }
  const confirmed = await confirmAction(
    `升级 ${deviceName(device)}？`,
    `当前版本 ${describe(device.firmware_version)} → ${item.version}。设备会在验证并写入固件后重启。请保持 USB 供电；首启自检确认前不会标为成功。`,
    '开始升级');
  if (!confirmed) return;
  state.busy = true;
  clearMessage();
  try {
    await ctx.api.request(`/devices/${encodeURIComponent(device.device_id)}/updates`, {
      method: 'POST', json: { package_id: item.package_id, expected_firmware_version: device.firmware_version },
    });
    await ctx.refresh();
    toast('升级任务已保存，等待设备接收');
  } catch (error) {
    showMessage(errorText(error), 'error', error.code);
  } finally {
    state.busy = false;
    ctx.render();
  }
}

async function upload(file, ui, ctx) {
  if (!file || state.busy) return;
  if (file.size === 0 || file.size > MAX_PACKAGE_BYTES) { showMessage('请选择不超过 7 MiB 的非空签名固件包。', 'error'); return; }
  state.busy = true;
  ui.submit.disabled = true;
  clearMessage();
  ui.progress.textContent = '正在上传…';
  try {
    await ctx.api.upload('/firmware', file, {
      onProgress: ratio => { ui.progress.textContent = `正在上传 ${Math.floor(ratio * 100)}%`; },
      onSent: () => { ui.progress.textContent = '正在验证签名与镜像…'; },
    });
    toast('固件签名与镜像验证通过，已入库');
    await ctx.refresh();
    ctx.render();
  } catch (error) {
    if (error.status !== 401) showMessage(errorText(error), 'error', error.code);
    ui.progress.textContent = '未入库';
  } finally {
    state.busy = false;
    ui.submit.disabled = false;
  }
}

function uploadPanel(ctx) {
  const input = h('input', { type: 'file', id: 'firmware-file', accept: '.dhfw,.zip,application/zip', 'aria-label': '选择签名固件包' });
  const progress = h('span', { id: 'upload-progress', class: 'small muted', role: 'status' });
  const submit = btn('验证并入库', 'primary', () => upload(input.files?.[0], { submit, progress }, ctx), { id: 'upload-submit', disabled: true });
  input.addEventListener('change', () => { submit.disabled = !input.files?.length; });
  const box = panel('上传签名固件包', '支持 .dhfw / .zip，最大 7 MiB');
  box.append(body(
    h('p', { class: 'muted' }, 'Agent 会检查受信签名、板卡、镜像版本和 SHA-256，全部通过才入库。USB 全量刷机包不能用于 OTA。'),
    h('div', { class: 'row' }, input, submit, progress)));
  return box;
}

function libraryPanel(ctx) {
  if (state.packages === null) {
    const loading = panel('固件库');
    loading.append(empty('正在读取', '正在向 Agent 读取固件库。'));
    return loading;
  }
  const box = panel('固件库', `${state.packages.length} 个通过验证的包`);
  if (!state.packages.length) {
    box.append(empty('固件库还是空的', '用本机签名工具生成升级包，再上传到这里。私钥不会交给管理网页或设备。'));
    return box;
  }
  const blocked = Boolean(updateBlocker(getDevice()));
  box.append(table(['固件版本', '硬件 / 大小', '来源验证', '入库时间', ''], state.packages.map(item => [
    h('div', {}, h('strong', {}, item.version), h('span', { class: 'cell-sub mono' }, `${String(item.package_id).slice(0, 14)}…`)),
    h('div', {}, item.board_model, h('span', { class: 'cell-sub' }, bytes(item.image_size))),
    badge('签名已验证', 'ok'),
    h('span', { class: 'small' }, shortTime(item.created_at)),
    btn('安装到设备', 'small', () => install(item, ctx), { disabled: blocked }),
  ])));
  return box;
}

export function renderFirmware(container, ctx) {
  const page = h('div', { class: 'page' }, pageHead('固件升级', '先验证来源，再交给设备。每一步结果都可追踪。', ...ctx.tools()));
  container.append(page);

  if (activeDevices().length) {
    const device = getDevice();
    const target = panel('升级目标', '只向最近上报、USB 供电、具备升级能力的设备发起任务');
    const blocker = updateBlocker(device);
    target.append(body(deviceSelector(ctx), blocker && !hasActiveUpdate(device) ? notice('现在不能升级', blocker, 'warn') : null, updateStatus(device)));
    page.append(target);
  } else {
    page.append(notice('还没有设备', '设备上报后才能发起升级。你仍然可以先把固件包入库。'));
  }
  page.append(uploadPanel(ctx), libraryPanel(ctx),
    notice('USB 仍是恢复入口', '设备无法启动或无法联网时，用已确认的下载端口恢复。不要把 erase-flash 当作普通升级步骤，它会删除配对与设置。'));
}
