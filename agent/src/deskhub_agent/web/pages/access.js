// 访问管理：设备凭据、配对新设备（USB、无线）、管理员会话、修改密码。
import { h } from '../lib/dom.js';
import { errorText } from '../lib/copy.js';
import { shortTime } from '../lib/format.js';
import { state } from '../lib/state.js';
import {
  btn, confirmAction, deviceIdentity, empty, fact, notice, onlineBadge, pageHead, panel, body, showMessage, snippet, table, toast, deviceName,
} from '../lib/ui.js';
import { provisioningSteps } from './common.js';

const MIN_PASSWORD = 12;
const SCOPE_TEXT = { 'read:status': '读状态', 'control:displays': '切换显示器', 'device:sync': '上报与配置' };
const GRANT_STATES = { pending: ['等待中', 'warn'], used: ['已使用', 'ok'], expired: ['已过期', 'neutral'], cancelled: ['已取消', 'neutral'], burned: ['已作废（输错太多次）', 'bad'] };

async function revoke(device, ctx) {
  const confirmed = await confirmAction(
    `吊销 ${deviceName(device)} 的凭据？`,
    '设备的后续认证请求会被拒绝。已经开始的硬件操作不保证能撤销；历史记录会保留。重新使用需要通过 USB 再次配对。',
    '吊销凭据', true);
  if (!confirmed) return;
  try {
    await ctx.api.request(`/devices/${encodeURIComponent(device.device_id)}/revoke`, { method: 'POST', json: {} });
    await ctx.refresh();
    toast('设备凭据已吊销');
    ctx.render();
  } catch (error) {
    showMessage(errorText(error), 'error', error.code);
  }
}

const chips = (values, none = '无') => h('div', { class: 'row' }, values?.length ? values.map(value => h('span', { class: 'chip' }, value)) : h('span', { class: 'muted' }, none));

// ---------- 无线配对：授权 ----------
async function loadGrants(ctx, area) {
  try {
    const data = await ctx.api.request('/pairing-grants');
    area.replaceChildren(...grantList(data.grants || [], ctx, area));
  } catch (error) {
    area.replaceChildren(h('p', { class: 'small muted' }, errorText(error)));
  }
}

function grantList(grants, ctx, area) {
  if (!grants.length) return [h('p', { class: 'small muted' }, '没有授权记录。')];
  return grants.slice(0, 8).map(grant => {
    const [text, kind] = GRANT_STATES[grant.state] || [grant.state, 'neutral'];
    return h('div', { class: 'channel', dataset: { grant: grant.grant_id } },
      h('strong', { class: 'mono' }, grant.grant_id),
      h('span', { class: 'note' }, `${grant.scopes.map(scope => SCOPE_TEXT[scope] || scope).join('、')} · 到期 ${shortTime(grant.expires_at)}${grant.state === 'pending' ? ` · 还可输错 ${grant.attempts_left} 次` : ''}`),
      h('span', { class: `chip ${kind === 'neutral' ? '' : kind}`.trim() }, text),
      grant.state === 'pending' ? btn('取消', 'small ghost', async () => {
        try {
          await ctx.api.request(`/pairing-grants/${encodeURIComponent(grant.grant_id)}`, { method: 'DELETE' });
          await loadGrants(ctx, area);
        } catch (error) {
          showMessage(errorText(error), 'error', error.code);
        }
      }) : null);
  });
}

function pairingPanel(ctx) {
  const box = panel('无线配对', '生成一次性的配对包，粘贴到设备的配网页');
  const scopes = Object.entries(SCOPE_TEXT).map(([scope, text]) => {
    const input = h('input', { type: 'checkbox', id: `grant-${scope}`, value: scope, checked: scope !== 'control:displays' });
    return { scope, input, label: h('label', { class: 'row', for: `grant-${scope}` }, input, text) };
  });
  const ttl = h('select', { id: 'grant-ttl', 'aria-label': '有效时间' },
    [[60, '1 分钟'], [180, '3 分钟'], [300, '5 分钟']].map(([value, text]) => h('option', { value: String(value) }, text)));
  ttl.value = '300';
  const deviceId = h('input', { type: 'text', id: 'grant-device', maxlength: 96, placeholder: '可不填：设备自己取名', autocomplete: 'off' });
  const replace = h('input', { type: 'checkbox', id: 'grant-replace' });
  const result = h('div', { id: 'grant-result', class: 'stack' });
  const list = h('div', { id: 'grant-list' });
  const create = btn('生成配对包', 'primary', async () => {
    const chosen = scopes.filter(item => item.input.checked).map(item => item.scope);
    if (!chosen.length) { showMessage('至少选一项权限。', 'error'); return; }
    // 切换显示器要指明目标；网页只提供 Agent 已配置的显示器。
    let targets = [];
    if (chosen.includes('control:displays')) {
      try {
        const data = state.displays || await ctx.api.request('/displays');
        targets = (data.displays || []).map(display => display.monitor_id).slice(0, 4);
      } catch (error) { showMessage(errorText(error), 'error', error.code); return; }
      if (!targets.length) { showMessage('Agent 里还没有配置显示器，不能授权切换显示器。', 'error'); return; }
    }
    create.disabled = true;
    try {
      const grant = await ctx.api.request('/pairing-grants', { method: 'POST', json: {
        scopes: chosen, targets, ttl_seconds: Number(ttl.value), replace: replace.checked, ...(deviceId.value.trim() ? { device_id: deviceId.value.trim() } : {}),
      } });
      result.replaceChildren(
        notice('配对包只显示这一次', `到 ${shortTime(grant.expires_at)} 前有效，只能用一次。CA 指纹（SHA-256）${grant.ca_fingerprint.slice(0, 16)}…`, 'warn'),
        snippet(grant.package));
      await loadGrants(ctx, list);
    } catch (error) {
      showMessage(errorText(error), 'error', error.code);
    } finally {
      create.disabled = false;
    }
  }, { id: 'create-grant' });
  box.append(body(
    h('ol', { class: 'steps' },
      h('li', {}, h('div', {}, h('h3', {}, '生成配对包'), h('p', {}, '选好权限，点“生成配对包”。包里有一次性验证码和 Agent 的 CA 证书。'))),
      h('li', {}, h('div', {}, h('h3', {}, '粘贴到设备的配网页'), h('p', {}, '在设备上长按按键开热点，用手机或电脑连上它，打开配网页，粘贴配对包。')))),
    h('div', { class: 'stack' },
      h('div', { class: 'field' }, h('span', { class: 'field-label' }, '权限'), h('div', { class: 'row' }, scopes.map(item => item.label))),
      h('div', { class: 'form-grid' },
        h('div', { class: 'field' }, h('label', { for: 'grant-ttl' }, '有效时间'), ttl),
        h('div', { class: 'field' }, h('label', { for: 'grant-device' }, '设备名'), deviceId)),
      h('label', { class: 'row', for: 'grant-replace' }, replace, '这个设备名已经配对过，换成新的凭据'),
      h('div', {}, create)),
    result,
    h('div', {}, h('h3', {}, '授权记录'), list)));
  loadGrants(ctx, list);
  return box;
}

// ---------- 管理员密码 ----------
function passwordPanel(ctx) {
  const current = h('input', { type: 'password', id: 'current-password', autocomplete: 'current-password', maxlength: 256 });
  const next = h('input', { type: 'password', id: 'new-password', autocomplete: 'new-password', maxlength: 256 });
  const again = h('input', { type: 'password', id: 'confirm-password', autocomplete: 'new-password', maxlength: 256 });
  const submit = btn('修改密码', 'primary', async () => {
    if ([...next.value].length < MIN_PASSWORD) { showMessage(`新密码至少 ${MIN_PASSWORD} 个字符。`, 'error'); return; }
    if (next.value !== again.value) { showMessage('两次输入的新密码不一致。', 'error'); return; }
    submit.disabled = true;
    try {
      const session = await ctx.api.request('/password', { method: 'PUT', json: { password: next.value, current_password: current.value } });
      state.session = session;
      state.csrf = session.csrf_token;
      current.value = next.value = again.value = '';
      showMessage('密码已修改。其他已登录的会话已经失效。');
    } catch (error) {
      current.value = '';
      showMessage(errorText(error), 'error', error.code);
    } finally {
      submit.disabled = false;
    }
  }, { id: 'change-password' });
  const box = panel('管理员账户', '网页只接受密码');
  box.append(body(
    h('div', { class: 'field' }, h('label', { for: 'current-password' }, '当前密码'), current),
    h('div', { class: 'field' }, h('label', { for: 'new-password' }, `新密码（至少 ${MIN_PASSWORD} 个字符）`), next),
    h('div', { class: 'field' }, h('label', { for: 'confirm-password' }, '确认新密码'), again),
    h('div', {}, submit),
    h('p', { class: 'small muted' }, '修改后，其他已登录的会话会立即失效。输错当前密码也算一次失败，连续 5 次会锁定网页登录 15 分钟。'),
    h('div', { class: 'callout' },
      h('div', {}, h('strong', {}, '口令'), '：已用于首次登录。现在只给命令行和脚本用。'),
      h('div', { class: 'small muted' }, '要吊销口令，在运行 Agent 的电脑上执行 deskhubctl revoke <口令名>。网页看不到口令。'))));
  return box;
}

export function renderAccess(container, ctx) {
  const page = h('div', { class: 'page' },
    pageHead('访问管理', '每台终端独立授权，可以单独吊销。吊销不能撤回已经发出的硬件操作。', ...ctx.tools()));

  const devices = panel('设备凭据', '吊销后保留配置与升级历史，设备后续请求会被拒绝');
  if (!state.devices.length) {
    devices.append(empty('还没有设备凭据', '通过本机工具完成配对后，可以在这里查看并吊销设备身份。'));
  } else {
    devices.append(table(['设备身份', '连接状态', '权限范围', '可控目标', '最近上报', ''], state.devices.map(device => [
      deviceIdentity(device),
      onlineBadge(device),
      chips(device.scopes),
      chips(device.targets.includes('*') ? ['全部目标'] : device.targets),
      h('span', { class: 'small' }, shortTime(device.last_seen)),
      btn(device.revoked ? '已吊销' : '吊销…', 'small danger', () => revoke(device, ctx), { disabled: device.revoked }),
    ])));
  }
  devices.append(body(notice('设备恢复出厂不等于远端凭据已吊销', '设备离线时，请在这里手动吊销旧身份。重新配对会生成新的随机凭据，旧凭据无法恢复或查看。', 'warn')));
  page.append(devices);

  const usb = panel('USB 配对', '凭据只在本机生成，经 USB 写入设备');
  usb.append(body(provisioningSteps()));

  const listen = state.overview?.listen;
  const session = panel('当前管理员会话', '会话只保存在服务器发的 HttpOnly 项里，30 分钟后过期');
  session.append(body(
    h('dl', { class: 'facts' },
      fact('身份', state.session?.admin_id), fact('会话到期', shortTime(state.session?.expires_at)), fact('管理地址', window.location.host),
      fact('监听', listen ? `${listen.bind}:${listen.port}` : null, true), fact('传输', listen ? (listen.tls ? 'HTTPS（TLS）' : '仅本机 HTTP') : null),
      fact('写操作校验', '同源 Origin + CSRF')),
    h('div', {}, btn('退出登录', '', () => ctx.signOut(), { id: 'sign-out' }))));

  page.append(h('div', { class: 'cols' }, h('div', { class: 'grow stack' }, usb, pairingPanel(ctx)), h('div', { class: 'side stack' }, session, passwordPanel(ctx))));
  container.append(page);
}
