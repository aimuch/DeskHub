#!/usr/bin/env node
// 管理台端到端测试：真实 Agent 进程 + 真实 Chrome（CDP，真实鼠标点击和键入）。
//
// 没有真机和显示器，所以有两处明确的替身，写进 report.json：
//   1. 设备上报由本脚本模拟（HTTP 调用的是真实的 /api/v1/device/sync）。
//   2. 显示器后端是 scripts/e2e/fake_betterdisplay.py，Agent 像调用真实 CLI 一样用子进程调用它。
// 用法：node scripts/e2e/console.e2e.mjs      输出：docs/evidence/console-e2e/{report.json,*.png}
// 全部状态在临时目录里；不碰仓库里的任何私有文件。
import { execFileSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Browser, sleep, waitUntil } from './cdp.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const PY = process.env.PYTHON || path.join(ROOT, '.venv/bin/python');
const CTL = process.env.DESKHUB_CTL || path.join(ROOT, '.venv/bin/deskhubctl');
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const OUT = process.env.E2E_OUT || path.join(ROOT, 'docs/evidence/console-e2e');

const checks = [];
function check(name, ok, detail = '') {
  checks.push({ name, ok: Boolean(ok), detail: String(detail) });
  console.log(`${ok ? '✓' : '✗'} ${name}${ok || !detail ? '' : ` — ${detail}`}`);
}
let failureShot = async () => '';
async function step(name, fn) {
  try { await fn(); } catch (error) { check(name, false, `${error.message} ${await failureShot(name)}`); }
}
const freePort = () => new Promise(resolve => { const server = net.createServer(); server.listen(0, '127.0.0.1', () => { const { port } = server.address(); server.close(() => resolve(port)); }); });

// ---------- 环境 ----------
fs.mkdirSync(OUT, { recursive: true });
for (const name of fs.readdirSync(OUT)) if (name.endsWith('.png')) fs.rmSync(path.join(OUT, name));
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'deskhub-console-e2e-'));
const config = path.join(work, 'agent.json');
const fakeState = path.join(work, 'bd');
fs.mkdirSync(fakeState);
const env = { ...process.env, FAKE_BD_STATE: fakeState };
const ctl = (...args) => execFileSync(CTL, ['--config', config, ...args], { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });

ctl('init');
const settings = JSON.parse(fs.readFileSync(config, 'utf8'));
const wrapper = path.join(work, 'fake-betterdisplay');
fs.writeFileSync(wrapper, `#!/bin/sh\nexec "${PY}" "${ROOT}/scripts/e2e/fake_betterdisplay.py" "$@"\n`, { mode: 0o700 });
const tls = path.join(work, 'tls');
execFileSync(PY, [path.join(ROOT, 'tools/create_tls.py'), '--out', tls, '--dns', 'localhost', '--ip', '127.0.0.1'], { stdio: 'ignore' });

const port = await freePort();
const base = `http://127.0.0.1:${port}`;
Object.assign(settings, {
  port, backend: 'betterdisplay', backend_path: wrapper, public_url: `https://127.0.0.1:${port}`, pairing_ca: path.join(tls, 'ca.pem'),
  displays: [{ monitor_id: 'monitor-desk-01', label: '书桌显示器', binding: { uuid: 'E2E-UUID-0001', serial: 'E2E123', registry_location: 'IOService:/e2e/monitor' }, inputs: { 'mac-usbc': 15, 'pc-dp': 17 }, readback_enabled: true }],
  actions: [
    { action_id: 'desktop.switch_to_mac', label: '切到 Mac', target_id: 'monitor-desk-01', input_id: 'mac-usbc' },
    { action_id: 'desktop.switch_to_pc', label: '切到 PC', target_id: 'monitor-desk-01', input_id: 'pc-dp' },
  ],
});
fs.writeFileSync(config, JSON.stringify(settings, null, 2), { mode: 0o600 });

const files = { admin: path.join(work, 'admin.json'), device: path.join(work, 'device.json'), spare: path.join(work, 'spare.json') };
ctl('admin-token', '--output', files.admin);
const scopes = ['--scope', 'read:status', '--scope', 'control:displays', '--scope', 'device:sync', '--target', 'monitor-desk-01'];
ctl('pair', 'desk-01', ...scopes, '--output', files.device);
ctl('pair', 'desk-02', '--scope', 'read:status', '--scope', 'device:sync', '--output', files.spare);
const token = JSON.parse(fs.readFileSync(files.admin, 'utf8')).token;
const deviceToken = JSON.parse(fs.readFileSync(files.device, 'utf8')).token;

const agentLog = fs.openSync(path.join(work, 'agent.out'), 'w');
const agent = spawn(CTL, ['--config', config, 'serve'], { env, stdio: ['ignore', agentLog, agentLog] });
await waitUntil(async () => (await request('GET', '/healthz')).status === 200, { what: 'Agent 启动' });

// ---------- 用 node:http 说话（可以设置 Origin，读 Set-Cookie） ----------
function request(method, route, { json, cookie, csrf, origin = base, headers = {} } = {}) {
  return new Promise((resolve, reject) => {
    const body = json === undefined ? null : JSON.stringify(json);
    const req = http.request(base + route, {
      method,
      headers: { ...(body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {}), ...(origin ? { Origin: origin } : {}),
        ...(cookie ? { Cookie: cookie } : {}), ...(csrf ? { 'X-CSRF-Token': csrf } : {}), ...headers },
    }, res => {
      let text = '';
      res.on('data', chunk => { text += chunk; });
      res.on('end', () => {
        let data = null;
        try { data = JSON.parse(text); } catch { /* 不是 JSON */ }
        const setCookie = (res.headers['set-cookie'] || []).map(value => value.split(';')[0]).find(value => value.startsWith('deskhub_session='));
        resolve({ status: res.statusCode, data, cookie: setCookie });
      });
    });
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}
const code = response => response.data?.error?.code;

// ---------- 模拟设备（真实的 /api/v1/device/sync） ----------
const device = {
  autoAck: false,
  report: {
    firmware_version: '0.3.4', board_model: 'ESP32-S3-Touch-AMOLED-2.16', boot_id: 'boot-e2e', uptime_seconds: 300000, free_heap_bytes: 180000,
    applied_revision: 0, configuration: { schema_version: 1, display: {}, layout: {} }, config_error: null,
    power: { vbus: 'present', battery: 'present', charging: 'charging', fault: false, soc_percent: 67, battery_mv: 4120, vbus_mv: 5030, chip_temp_c: 42.0, measurement: 'ok' },
    capabilities: { management: true, signed_ota: false, layouts: ['desk', 'clock', 'focus'], update_key_fingerprint: null }, update: null,
  },
};
async function heartbeat() {
  const response = await request('POST', '/api/v1/device/sync', { json: device.report, headers: { Authorization: `Bearer ${deviceToken}` }, origin: null });
  if (device.autoAck && response.data?.desired) {
    device.report.applied_revision = response.data.desired.revision;
    device.report.configuration = response.data.desired.configuration;
    return heartbeat();
  }
  return response;
}
await heartbeat();
const beat = setInterval(() => heartbeat().catch(() => {}), 8000);

// ---------- 浏览器 ----------
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'deskhub-chrome-'));
const browser = await Browser.launch({ chrome: CHROME, port: await freePort(), profile });
failureShot = async name => {
  const file = path.join(OUT, `FAILED-${name.replace(/[^\w一-龥]+/g, '_').slice(0, 40)}.png`);
  try { await browser.screenshot(file); } catch { return ''; }
  const hint = await browser.evaluate('document.querySelector(".message:not([hidden]), [role=alert]:not([hidden])")?.innerText ?? ""').catch(() => '');
  return `（截图 ${path.basename(file)}；页面提示：${hint}）`;
};
await browser.setViewport(1280, 900);
let shot = 0;
const snap = async (name, opts) => { shot += 1; await browser.screenshot(path.join(OUT, `${String(shot).padStart(2, '0')}-${name}.png`), opts); };
const visible = id => browser.evaluate(`Boolean(document.getElementById(${JSON.stringify(id)})) && !document.getElementById(${JSON.stringify(id)}).hidden`);
const heading = () => browser.evaluate('document.querySelector("#content h1")?.innerText ?? ""');
const openPage = async (key, title) => {
  await browser.click(`.nav-item[data-page="${key}"]`);
  await browser.waitFor(`document.querySelector("#content h1")?.innerText === ${JSON.stringify(title)}`, `进入“${title}”页`);
};
const waitText = (text, what = text, timeout) => browser.waitFor(`document.body.innerText.includes(${JSON.stringify(text)})`, what, timeout);
const waitGone = (text, what = text) => browser.waitFor(`!document.body.innerText.includes(${JSON.stringify(text)})`, what);
const PASSWORD = 'q7Z!mK2#pL9v';
const NEXT_PASSWORD = 'n4W$xT8&cB5r';
const AGAIN_PASSWORD = 'h6K%dF3@yR8w';

try {
  // ===== 1. 第一次打开：登录页 =====
  await step('登录页', async () => {
    await browser.goto(`${base}/manage/`);
    await browser.waitFor('!document.getElementById("login-view").hidden', '登录页出现');
    check('没有设置过密码时，默认停在“口令（首次）”', await browser.attr('#tab-token', 'aria-selected') === 'true' && await visible('token-form'));
    check('登录页里没有控制台内容', !await visible('application') && !await visible('setup-view'));
    await snap('login-first');
    await browser.type('#login-token', 'x'.repeat(30));
    await browser.click('#token-submit');
    await waitText('口令无效', '错误的口令被拒绝');
    check('错误的口令：只给一句话，输入框被清空', await browser.evaluate('document.getElementById("login-token").value === ""'));
    await browser.click('#tab-password');
    await browser.type('#login-password', PASSWORD);
    await browser.click('#password-submit');
    await waitText('密码不对', '没有设置密码时，密码登录得到同样的回答');
    check('没有设置过密码时，密码登录和密码错的回答一样', true);
  });

  // ===== 2. 口令登录 → 只能设置密码 =====
  await step('口令登录后只能设置密码', async () => {
    await browser.click('#tab-token');
    await browser.type('#login-token', token);
    await browser.click('#token-submit');
    await browser.waitFor('!document.getElementById("setup-view").hidden', '设置密码页出现');
    check('口令登录后进入“设置密码”，不是控制台', await visible('setup-view') && !await visible('application'));
    const overview = await browser.evaluate('fetch("/api/v1/admin/overview").then(async r => ({ status: r.status, body: await r.json() }))');
    check('设置会话访问其他管理接口：403 PASSWORD_REQUIRED', overview.status === 403 && overview.body.error.code === 'PASSWORD_REQUIRED', JSON.stringify(overview));
    const upload = await browser.evaluate('fetch("/api/v1/admin/firmware", { method: "POST", headers: { "Content-Type": "application/zip" }, body: new Blob(["PK"]) }).then(r => r.status)');
    check('设置会话不能上传固件', upload === 403, upload);
    await browser.goto(`${base}/manage/`);
    await browser.waitFor('!document.getElementById("setup-view").hidden', '重新载入后仍在设置密码页');
    check('重新载入页面，仍然停在“设置密码”', await visible('setup-view'));
    await snap('setup-password');
  });

  // ===== 3. 设置密码 =====
  await step('设置密码', async () => {
    check('一开始“设置密码并进入”不可点', await browser.evaluate('document.getElementById("setup-submit").disabled'));
    await browser.type('#setup-password', 'short');
    await browser.type('#setup-confirm', 'short');
    check('太短时仍不可点', await browser.evaluate('document.getElementById("setup-submit").disabled'));
    await browser.type('#setup-password', 'password1234');
    await browser.type('#setup-confirm', 'password1234');
    check('长度够且两次一致时可点', await browser.evaluate('!document.getElementById("setup-submit").disabled'));
    await browser.click('#setup-submit');
    await waitText('太弱', '常见的弱密码被服务器拒绝');
    check('弱密码被拒绝，仍停在设置页，输入框里没有回显', await visible('setup-view') && !await visible('application'));
    await browser.type('#setup-password', PASSWORD);
    await browser.type('#setup-confirm', 'different-1234');
    check('两次不一致时不可点', await browser.evaluate('document.getElementById("setup-submit").disabled'));
    await browser.type('#setup-confirm', PASSWORD);
    await browser.click('#setup-submit');
    await browser.waitFor('!document.getElementById("application").hidden && document.querySelector("#content h1")?.innerText === "概览"', '设置完成，进入概览');
    check('设置密码后进入控制台（概览）', true);
    check('会话 Cookie 对页面脚本不可见（HttpOnly）', await browser.evaluate('!document.cookie.includes("deskhub_session")'));
    const again = await request('POST', '/api/v1/admin/session', { json: { token } });
    check('设置密码后，带 Origin 的口令登录被拒绝：403 TOKEN_LOGIN_DISABLED', again.status === 403 && code(again) === 'TOKEN_LOGIN_DISABLED', `${again.status} ${code(again)}`);
  });

  // ===== 4. 九个页面 =====
  await step('概览', async () => {
    await heartbeat();
    await browser.click('#refresh');
    await waitText('67%', '概览里有电量');
    const text = await browser.text();
    check('概览：设备在线、电量、充电中', text.includes('在线') && text.includes('67%') && text.includes('充电中'));
    check('概览：状态环有说明文字', (await browser.attr('.hero .ring', 'aria-label') || '').startsWith('状态环：'));
    check('概览：四块色卡', await browser.count('.tile') === 4);
    await snap('overview', { full: true });
  });

  const pages = [['devices', '设备', 'desk-01'], ['actions', '动作与显示器', '书桌显示器'], ['commands', '命令记录', '最近命令'], ['layout', '布局与显示', '主题与方向'],
    ['power', '电源', '实时读数'], ['firmware', '固件升级', '上传签名固件包'], ['access', '访问管理', '设备凭据'], ['appearance', '外观', '管理台']];
  for (const [key, title, mustContain] of pages) {
    await step(`页面：${title}`, async () => {
      await openPage(key, title);
      await waitText(mustContain, `“${title}”页内容`);
      check(`页面“${title}”打开，内容在`, true);
      if (['devices', 'actions', 'power', 'access'].includes(key)) await snap(key, { full: true });
    });
  }

  // ===== 5. 命令（真实命令行 → 假显示器后端） =====
  await step('命令记录', async () => {
    const run = execFileSync(CTL, ['--config', config, 'run', 'desktop.switch_to_mac', '--credentials', files.device, '--url', base, '--wait'], { env, encoding: 'utf8' });
    check('命令行执行显示器动作：已确认', JSON.parse(run).state === 'succeeded', run.slice(0, 120));
    await openPage('overview', '概览');
    await openPage('commands', '命令记录');
    await waitText('desktop.switch_to_mac', '命令出现在记录里');
    await browser.click('.choice[data-filter="failed"]');
    await waitText('这个筛选下没有命令', '“失败”筛选下为空');
    await browser.click('.choice[data-filter="succeeded"]');
    await waitText('desktop.switch_to_mac');
    await browser.click('button[data-command]');
    await waitText('读回输入源', '命令细节');
    check('命令记录：筛选和细节', true);
    await snap('commands', { full: true });
  });

  // ===== 6. 布局：保存 → 等待 → 设备确认 =====
  await step('布局与显示', async () => {
    await openPage('layout', '布局与显示');
    await browser.click('.option[data-theme="ocean"]');
    await browser.click('#rotate-screen');
    check('预览跟着主题与方向变', await browser.attr('#layout-preview', 'data-theme') === 'ocean' && await browser.attr('#layout-preview', 'data-rotation') === '90');
    await snap('layout-draft', { full: true });
    await browser.click('#save-configuration');
    await waitText('配置已保存在 Agent', '保存成功提示');
    await openPage('devices', '设备');
    await waitText('等待设备确认', '设备页：等待设备确认');
    const badge = () => browser.evaluate(`(() => { const panel = [...document.querySelectorAll('#content .panel')].find(item => item.querySelector('h2')?.innerText === '配置状态'); const mark = panel?.querySelector('.badge'); return mark ? { text: mark.innerText, kind: mark.className } : null; })()`);
    const waiting = await badge();
    check('保存后，设备页的配置状态是“等待设备确认”（黄色），不是“设备已应用”', waiting && waiting.text === '等待设备确认' && waiting.kind.includes('warn'), JSON.stringify(waiting));
    device.autoAck = true;
    await heartbeat();
    await browser.click('#refresh');
    await browser.waitFor('document.body.innerText.includes("设备已应用")', '设备确认后显示“设备已应用”');
    const applied = await badge();
    check('设备上报同一个 revision 和完整配置后，才显示“设备已应用”（绿色）', applied && applied.text === '设备已应用' && applied.kind.includes('ok'), JSON.stringify(applied));
    await snap('devices-applied', { full: true });
  });

  // ===== 7. 电源 =====
  await step('电源', async () => {
    await openPage('power', '电源');
    const text = await browser.text();
    check('电源：电量、电压、温度', text.includes('67%') && text.includes('4.12 V') && text.includes('5.03 V') && text.includes('42.0 °C'));
    check('电源：充电保护不放假开关', text.includes('网页暂时不能开关它') && !await browser.exists('#content [role="switch"]'));
    await snap('power', { full: true });
  });

  // ===== 8. 固件 =====
  await step('固件', async () => {
    await openPage('firmware', '固件升级');
    const bogus = path.join(work, 'bogus.zip');
    fs.writeFileSync(bogus, 'not a package');
    await browser.setFiles('#firmware-file', [bogus]);
    await browser.click('#upload-submit');
    await browser.waitFor('document.getElementById("upload-progress")?.innerText === "未入库"', '无效固件包被拒绝');
    check('固件：无效的包被拒绝，没有入库', (await browser.text()).includes('固件库还是空的'));
  });

  // ===== 9. 访问：配对授权、吊销、修改密码 =====
  await step('访问管理', async () => {
    await openPage('access', '访问管理');
    await browser.click('#create-grant');
    await browser.waitFor('document.querySelector("#grant-result pre")?.innerText.startsWith("DHP2.")', '生成配对包');
    check('无线配对：生成配对包（DHP2.…），只显示一次', true);
    await waitText('等待中', '授权记录里是“等待中”');
    await browser.click('#grant-list button');
    await browser.waitFor('document.querySelector("#grant-list")?.innerText.includes("已取消")', '取消授权');
    check('无线配对：取消授权', true);
    await browser.click('.btn.danger', 1);   // 第二行是 desk-02；desk-01 要留着在线
    await browser.waitFor('document.getElementById("confirm-dialog").open', '吊销确认框（取消）');
    await browser.click('#confirm-dialog button[value="cancel"]');
    await sleep(500);
    // 页脚的提示里本来就有“已吊销”三个字，所以只看 desk-02 那一行。
    const rowOf = id => browser.evaluate(`[...document.querySelectorAll('#content tbody tr')].find(tr => tr.innerText.includes(${JSON.stringify(id)}))?.innerText ?? ''`);
    const before = await rowOf('desk-02');
    check('在确认框里点“取消”：desk-02 没有被吊销', before.includes('desk-02') && !before.includes('已吊销'), before);
    await browser.click('.btn.danger', 1);
    await browser.waitFor('document.getElementById("confirm-dialog").open', '吊销确认框');
    check('吊销前有确认框，写明是哪一台', (await browser.textOf('#confirm-title') || '').includes('desk-02'));
    await browser.click('#confirm-submit');
    await browser.waitFor(`[...document.querySelectorAll('#content tbody tr')].find(tr => tr.innerText.includes('desk-02'))?.innerText.includes('已吊销')`, 'desk-02 被吊销');
    const after = await rowOf('desk-02');
    const first = await rowOf('desk-01');
    check('吊销 desk-02 之后，它那一行显示“已吊销”，desk-01 没有', after.includes('已吊销') && !first.includes('已吊销'), after);
  });

  await step('修改密码', async () => {
    const second = await request('POST', '/api/v1/admin/login', { json: { password: PASSWORD } });
    check('另一个会话（密码登录）可以读概览', second.status === 200 && (await request('GET', '/api/v1/admin/overview', { cookie: second.cookie })).status === 200);
    await browser.type('#current-password', 'wrong-current-1');
    await browser.type('#new-password', NEXT_PASSWORD);
    await browser.type('#confirm-password', NEXT_PASSWORD);
    await browser.click('#change-password');
    await waitText('密码不对', '当前密码错');
    await browser.type('#current-password', PASSWORD);
    await browser.type('#new-password', NEXT_PASSWORD);
    await browser.type('#confirm-password', 'something-else-1');
    await browser.click('#change-password');
    await waitText('不一致', '两次不一致');
    await browser.type('#current-password', PASSWORD);
    await browser.type('#new-password', 'password-123456');
    await browser.type('#confirm-password', 'password-123456');
    await browser.click('#change-password');
    await waitText('太弱', '弱密码被拒绝');
    await browser.type('#current-password', PASSWORD);
    await browser.type('#new-password', NEXT_PASSWORD);
    await browser.type('#confirm-password', NEXT_PASSWORD);
    await browser.click('#change-password');
    await waitText('其他已登录的会话已经失效', '密码修改成功');
    check('修改密码：错的当前密码、不一致、弱密码都被拒绝；正确的通过', true);
    check('修改密码后，另一个会话立即失效', (await request('GET', '/api/v1/admin/overview', { cookie: second.cookie })).status === 401);
    await browser.click('#refresh');
    await sleep(400);
    check('修改密码后，这个浏览器的会话继续有效（新 Cookie）', await visible('application'));
    const old = await request('POST', '/api/v1/admin/login', { json: { password: PASSWORD } });
    check('旧密码不能再登录', old.status === 401 && code(old) === 'LOGIN_FAILED');
  });

  // ===== 10. 外观 =====
  await step('外观', async () => {
    await openPage('appearance', '外观');
    await browser.click('.choice[data-name="theme"][data-value="light"]');
    await browser.waitFor('document.documentElement.dataset.theme === "light"', '切到浅色');
    await browser.click('.accent-dot[data-accent="sky"]');
    await browser.waitFor('document.documentElement.dataset.accent === "sky"', '强调色天蓝');
    await browser.setValue('#text-size', 17);
    await browser.waitFor('document.documentElement.dataset.size === "17"', '文字 17 px');
    check('外观：主题、强调色、文字大小立即生效', true);
    check('外观：对比度提示', (await browser.textOf('#contrast-line') || '').includes('通过'));
    await snap('appearance-light', { full: true });
    await browser.goto(`${base}/manage/#appearance`);
    await browser.waitFor('document.documentElement.dataset.theme === "light" && document.documentElement.dataset.accent === "sky"', '重新载入后外观还在');
    check('外观：重新载入后还在', true);
    const stored = await browser.evaluate('JSON.stringify({ local: Object.keys(localStorage), session: Object.keys(sessionStorage), value: localStorage.getItem("deskhub.appearance") })');
    const parsed = JSON.parse(stored);
    check('浏览器里只存外观：一个键，没有任何登录信息', parsed.local.length === 1 && parsed.local[0] === 'deskhub.appearance' && parsed.session.length === 0
      && !/token|password|csrf|session/i.test(parsed.value), stored);
    const trip = await browser.evaluate(`import('/manage/lib/prefs.js').then(prefs => {
      const text = prefs.exportText(prefs.load());
      return { roundTrip: JSON.stringify(prefs.parseImport(text)) === JSON.stringify(prefs.load()),
               badValue: prefs.parseImport('{"deskhub_appearance":1,"theme":"neon","accent":"mint","font":"system","density":"standard","motion":"on","size":15}'),
               extra: prefs.parseImport(JSON.stringify({ ...JSON.parse(text), token: 'x' })), notJson: prefs.parseImport('nope') };
    })`);
    check('外观：导出再导入一致；不合法的值、多余的项、不是 JSON 的文件都被拒绝', trip.roundTrip && trip.badValue === null && trip.extra === null && trip.notJson === null, JSON.stringify(trip));
    await openPage('appearance', '外观');
    await browser.click('#reset-button');
    await browser.waitFor('document.documentElement.dataset.theme === "system" && document.documentElement.dataset.size === "15"', '恢复默认');
    check('外观：恢复默认', true);
    await browser.setColorScheme('dark');
    await sleep(200);
    await snap('appearance-dark', { full: true });
  });

  // ===== 11. 退出、锁定、命令行重置 =====
  await step('退出与锁定', async () => {
    const good = await request('POST', '/api/v1/admin/login', { json: { password: NEXT_PASSWORD } });
    check('成功的登录把之前的失败次数清零', good.status === 200);
    await openPage('access', '访问管理');
    await browser.click('#sign-out');
    await browser.waitFor('!document.getElementById("login-view").hidden', '退出后回到登录页');
    check('退出后回到登录页，默认是“密码登录”', await visible('login-view') && await browser.attr('#tab-password', 'aria-selected') === 'true');
    for (let attempt = 1; attempt <= 5; attempt++) {
      await browser.type('#login-password', `wrong-password-${attempt}`);
      await browser.click('#password-submit');
      await browser.waitFor(`document.getElementById("password-error").innerText.includes("密码不对")`, `第 ${attempt} 次输错`);
      await browser.evaluate('document.getElementById("login-password").value = ""');
    }
    await browser.type('#login-password', NEXT_PASSWORD);
    await browser.click('#password-submit');
    await waitText('锁定', '第 6 次：账户锁定');
    check('连续输错 5 次后，账户锁定；这时输对密码也进不去', !await visible('application'));
    await snap('login-locked');
  });

  await step('命令行重置', async () => {
    ctl('admin-password-reset');
    await browser.goto(`${base}/manage/`);
    await browser.waitFor('!document.getElementById("login-view").hidden', '登录页');
    await browser.waitFor('document.getElementById("tab-token").getAttribute("aria-selected") === "true"', '重置后默认回到口令');
    check('命令行重置后：锁定解除，默认回到“口令（首次）”', true);
    await browser.type('#login-token', token);
    await browser.click('#token-submit');
    await browser.waitFor('!document.getElementById("setup-view").hidden', '重新设置密码');
    await browser.type('#setup-password', AGAIN_PASSWORD);
    await browser.type('#setup-confirm', AGAIN_PASSWORD);
    await browser.click('#setup-submit');
    await browser.waitFor('!document.getElementById("application").hidden', '重新进入控制台');
    check('重置后用口令登录，重新设置密码，进入控制台', true);
    ctl('admin-password-reset');
    await browser.click('#refresh');
    await browser.waitFor('!document.getElementById("login-view").hidden', '重置结束已打开的会话');
    check('命令行重置会结束已经打开的网页会话', true);
    await waitText('会话已失效', '登录页写明原因');
  });

  // ===== 12. 窄屏 =====
  await step('窄屏', async () => {
    await browser.type('#login-token', token);
    await browser.click('#token-submit');
    await browser.waitFor('!document.getElementById("setup-view").hidden', '设置密码页');
    await browser.setViewport(390, 800);
    await sleep(300);
    check('窄屏：设置密码页没有横向滚动条', await browser.evaluate('document.documentElement.scrollWidth <= window.innerWidth'));
    await snap('setup-mobile', { full: true });
    await browser.type('#setup-password', PASSWORD);
    await browser.type('#setup-confirm', PASSWORD);
    await browser.click('#setup-submit');
    await browser.waitFor('!document.getElementById("application").hidden', '进入控制台');
    await heartbeat();
    await browser.click('#refresh');
    await waitText('67%');
    check('窄屏：概览没有横向滚动条', await browser.evaluate('document.documentElement.scrollWidth <= window.innerWidth'));
    await snap('overview-mobile', { full: true });
    await openPage('layout', '布局与显示');
    check('窄屏：布局页没有横向滚动条', await browser.evaluate('document.documentElement.scrollWidth <= window.innerWidth'));
    await browser.setViewport(1280, 900);
  });

  // ===== 13. 设备不再上报：数据要变成“已过期”，不能一直显示旧数字 =====
  await step('数据过期', async () => {
    clearInterval(beat);
    await openPage('overview', '概览');
    await sleep(32000);   // Agent 把“最近 30 秒有上报”当作在线
    await browser.click('#refresh');
    await waitText('数据已过期', '电源卡片写“数据已过期”');
    const text = await browser.text();
    check('超过 30 秒没有上报：设备离线，电源写“数据已过期”，不显示旧的 67%', text.includes('离线') && !text.includes('67%'));
    await openPage('power', '电源');
    const power = await browser.text();
    check('电源页：过期后读数写“未上报”，不显示旧数字', !power.includes('4.12 V') && !power.includes('42.0 °C') && power.includes('未上报'));
    await snap('power-stale', { full: true });
  });

  check('全程没有脚本异常、CSP 违规或控制台错误', browser.problems.length === 0, JSON.stringify(browser.problems.slice(0, 3)));
} catch (error) {
  check('测试运行到结尾', false, `${error.stack || error.message} ${await failureShot('fatal')}`);
} finally {
  clearInterval(beat);
  const failed = checks.filter(item => !item.ok);
  const report = {
    kind: 'console-e2e', finished_at: new Date().toISOString(), chrome: browser.version, node: process.version,
    stand_ins: ['设备上报由脚本模拟（真实的 /api/v1/device/sync）', '显示器后端是 scripts/e2e/fake_betterdisplay.py（真实的子进程调用）'],
    not_covered: ['真实设备', '真实显示器', 'HTTPS（本测试用本机 HTTP，Cookie 的 Secure 标志在 pytest 里测）', '其他浏览器（只用了 Chrome）'],
    total: checks.length, passed: checks.length - failed.length, failed: failed.length, checks,
  };
  fs.writeFileSync(path.join(OUT, 'report.json'), `${JSON.stringify(report, null, 2)}\n`);
  browser.close();
  agent.kill('SIGTERM');
  console.log(`\n${report.passed}/${report.total} 通过。证据：${path.relative(ROOT, OUT)}/`);
  await sleep(800);   // Chrome 还在收尾写它的配置目录；等它退出再删，删不掉也不算测试失败
  for (const directory of [work, profile]) {
    try { fs.rmSync(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 }); } catch { /* 临时目录，系统会清理 */ }
  }
  process.exit(failed.length ? 1 : 0);
}
