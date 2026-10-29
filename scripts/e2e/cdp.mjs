// 零依赖的 Chrome DevTools Protocol 驱动，用于真实浏览器 E2E。需要 Node 22+（全局 WebSocket / fetch）。
import { spawn } from 'node:child_process';
import fs from 'node:fs';

export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function waitUntil(fn, { timeout = 15000, interval = 150, what = '条件' } = {}) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    try {
      const value = await fn();
      if (value) return value;
    } catch { /* 继续等待 */ }
    await sleep(interval);
  }
  throw new Error(`等待超时：${what}`);
}

export class Browser {
  static async launch({ chrome, port, profile }) {
    const child = spawn(chrome, [
      '--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
      '--no-first-run', '--no-default-browser-check', '--window-size=1280,900', 'about:blank',
    ], { stdio: 'ignore' });
    const version = await waitUntil(async () => (await fetch(`http://127.0.0.1:${port}/json/version`)).json(), { what: 'Chrome 启动' });
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const browser = new Browser(child, version, targets.find(target => target.type === 'page').webSocketDebuggerUrl);
    await browser.connect();
    return browser;
  }

  constructor(child, version, url) {
    this.child = child;
    this.version = version.Browser;
    this.url = url;
    this.pending = new Map();
    this.nextId = 1;
    this.problems = []; // 控制台错误、未捕获异常、CSP 违规
  }

  async connect() {
    this.socket = new WebSocket(this.url);
    await new Promise((resolve, reject) => { this.socket.onopen = resolve; this.socket.onerror = () => reject(new Error('无法连接 Chrome DevTools')); });
    this.socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.id) {
        const waiter = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) waiter.reject(new Error(`${waiter.method}: ${message.error.message}`));
        else waiter.resolve(message.result);
      } else this.onEvent(message);
    };
    for (const domain of ['Page', 'Runtime', 'Log', 'DOM']) await this.send(`${domain}.enable`);
  }

  onEvent({ method, params }) {
    if (method === 'Runtime.exceptionThrown') this.problems.push({ kind: 'exception', text: params.exceptionDetails.exception?.description || params.exceptionDetails.text });
    // 网络层 4xx（例如故意输错的登录）是预期内的；其余错误一律记录。
    if (method === 'Log.entryAdded' && ['error', 'warning'].includes(params.entry.level) && params.entry.source !== 'network') {
      this.problems.push({ kind: params.entry.source, text: params.entry.text });
    }
    if (method === 'Runtime.consoleAPICalled' && params.type === 'error') {
      this.problems.push({ kind: 'console', text: params.args.map(arg => arg.value ?? arg.description).join(' ') });
    }
  }

  send(method, params = {}) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  async evaluate(expression) {
    const result = await this.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (result.exceptionDetails) throw new Error(`页面脚本异常：${result.exceptionDetails.exception?.description || result.exceptionDetails.text}`);
    return result.result.value;
  }

  async goto(url) {
    await this.send('Page.navigate', { url });
    await waitUntil(() => this.evaluate('document.readyState === "complete"'), { what: `加载 ${url}` });
  }

  waitFor(expression, what, timeout = 15000) {
    return waitUntil(() => this.evaluate(expression), { what, timeout });
  }

  text() { return this.evaluate('document.body.innerText'); }
  textOf(selector) { return this.evaluate(`document.querySelector(${JSON.stringify(selector)})?.innerText ?? null`); }
  exists(selector) { return this.evaluate(`Boolean(document.querySelector(${JSON.stringify(selector)}))`); }
  count(selector) { return this.evaluate(`document.querySelectorAll(${JSON.stringify(selector)}).length`); }
  attr(selector, name) { return this.evaluate(`document.querySelector(${JSON.stringify(selector)})?.getAttribute(${JSON.stringify(name)}) ?? null`); }

  /** 真实鼠标点击：先滚动到可见，再确认点击点上的元素确实是它（不是被别的元素盖住）。 */
  async click(selector, index = 0) {
    const target = await this.evaluate(`(() => {
      const el = document.querySelectorAll(${JSON.stringify(selector)})[${index}];
      if (!el) return { missing: true };
      el.scrollIntoView({ block: 'center', inline: 'center' });
      const r = el.getBoundingClientRect();
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      const top = document.elementFromPoint(x, y);
      return { x, y, covered: !(top && (top === el || el.contains(top))), disabled: el.disabled === true };
    })()`);
    if (target.missing) throw new Error(`找不到元素 ${selector}`);
    if (target.disabled) throw new Error(`元素不可点击（disabled）：${selector}`);
    if (target.covered) throw new Error(`元素被其他内容遮挡：${selector}`);
    for (const type of ['mousePressed', 'mouseReleased']) {
      await this.send('Input.dispatchMouseEvent', { type, x: target.x, y: target.y, button: 'left', clickCount: 1 });
    }
    await sleep(60);
  }

  async type(selector, text) {
    await this.evaluate(`(() => { const el = document.querySelector(${JSON.stringify(selector)}); el.focus(); el.select?.(); })()`);
    await this.send('Input.insertText', { text });
  }

  /** 给 select / range / time 等无法逐字键入的控件赋值，并触发与用户操作相同的事件。 */
  setValue(selector, value, events = ['input', 'change']) {
    return this.evaluate(`(() => {
      const el = document.querySelector(${JSON.stringify(selector)});
      el.value = ${JSON.stringify(String(value))};
      for (const type of ${JSON.stringify(events)}) el.dispatchEvent(new Event(type, { bubbles: true }));
    })()`);
  }

  async setFiles(selector, files) {
    const { root } = await this.send('DOM.getDocument', { depth: 0 });
    const { nodeId } = await this.send('DOM.querySelector', { nodeId: root.nodeId, selector });
    await this.send('DOM.setFileInputFiles', { files, nodeId });
  }

  /** full: 临时把视口拉到内容高度再截，这样 sticky / 100vh 的元素（侧栏、保存栏）位置与真实浏览一致。 */
  async screenshot(file, { full = false } = {}) {
    const size = this.viewport || { width: 1280, height: 900 };
    if (full) {
      const height = await this.evaluate('Math.ceil(document.documentElement.scrollHeight)');
      await this.applyViewport(size.width, Math.min(Math.max(height, size.height), 2600));
      await sleep(250);
    }
    const { data } = await this.send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
    if (full) await this.applyViewport(size.width, size.height);
  }

  setColorScheme(value) {
    return this.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value }] });
  }

  applyViewport(width, height) {
    return this.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
  }

  /** 设置并记住视口；整页截图临时改尺寸后会还原到这里记住的值。 */
  setViewport(width, height) {
    this.viewport = { width, height };
    return this.applyViewport(width, height);
  }

  close() {
    try { this.socket?.close(); } catch { /* 已关闭 */ }
    this.child.kill('SIGTERM');
  }
}
