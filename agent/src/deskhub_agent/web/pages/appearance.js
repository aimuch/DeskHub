// 外观：管理台的语言、主题、强调色、字体、文字大小、密度和动效，只保存在这个浏览器里。
// 设备的外观受字库和内存限制，大部分在设备上改；这一页只读地说明设备能改什么。
import { h } from '../lib/dom.js';
import { THEMES } from '../lib/copy.js';
import { CHOICES, contrast, DEFAULTS, SIZES } from '../lib/prefs.js';
import { getDevice } from '../lib/state.js';
import { body, btn, panel, pageHead, showMessage, toast } from '../lib/ui.js';

const rootValue = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

/** 强调色用作文字（链接、选中项）时的对比度，和按钮上文字的对比度。低于 4.5 : 1 会警告。 */
function contrastLine() {
  let onCard = 0;
  let onButton = 0;
  try {
    onCard = contrast(rootValue('--accent'), rootValue('--card'));
    onButton = contrast(rootValue('--on-accent'), rootValue('--accent'));
  } catch { /* 取不到颜色时不显示数字 */ }
  const worst = Math.min(onCard, onButton);
  const ok = worst >= 4.5;
  return h('div', { class: 'row small', id: 'contrast-line' },
    h('span', { class: `chip ${ok ? 'ok' : 'warn'}` }, ok ? '通过' : '偏低'),
    h('span', {}, worst ? `文字对比度 ${onCard.toFixed(1)} : 1（强调色在卡片上）、${onButton.toFixed(1)} : 1（按钮文字）` : '对比度无法计算'),
    h('span', { class: 'muted' }, '低于 4.5 : 1 会警告'));
}

function choiceRow(label, name, ctx, update, current, extra = null) {
  return h('div', { class: 'choice-row' },
    h('div', { class: 'label' }, label),
    h('div', { class: 'choices', role: 'group', 'aria-label': label },
      CHOICES[name].map(([id, text]) => h('button', {
        type: 'button', class: 'choice', dataset: { name, value: id }, 'aria-pressed': String(current[name] === id), onclick: () => update({ [name]: id }),
      }, text)), extra));
}

export function renderAppearance(container, ctx) {
  const { prefs } = ctx;
  const current = prefs.load();
  const update = change => { prefs.apply(prefs.save({ ...prefs.load(), ...change })); ctx.render(); };

  const importInput = h('input', { type: 'file', id: 'import-file', accept: 'application/json,.json', hidden: true, 'aria-label': '导入外观文件' });
  importInput.addEventListener('change', async () => {
    const file = importInput.files?.[0];
    importInput.value = '';
    if (!file) return;
    const imported = file.size <= 4096 ? prefs.parseImport(await file.text()) : null;
    if (!imported) { showMessage('这个文件不是 DeskHub 的外观文件，没有改动任何选择。', 'error'); return; }
    update(imported);
    toast('外观已导入');
  });
  const exportFile = () => {
    const link = h('a', { href: URL.createObjectURL(new Blob([prefs.exportText(prefs.load())], { type: 'application/json' })), download: 'deskhub-appearance.json' });
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  };

  const size = h('input', { type: 'range', id: 'text-size', min: SIZES.min, max: SIZES.max, step: 1, value: String(current.size), 'aria-label': '文字大小' });
  const sizeText = h('output', { for: 'text-size' }, `${current.size} px`);
  size.addEventListener('input', () => { sizeText.textContent = `${size.value} px`; prefs.apply({ ...prefs.load(), size: Number(size.value) }); });
  size.addEventListener('change', () => update({ size: Number(size.value) }));

  const accents = h('div', { class: 'accents', role: 'group', 'aria-label': '强调色' },
    CHOICES.accent.map(([id, text]) => h('button', {
      type: 'button', class: 'accent-dot', dataset: { accent: id }, 'aria-label': text, title: text, 'aria-pressed': String(current.accent === id), onclick: () => update({ accent: id }),
    })));

  const console_ = panel('管理台', '保存在这个浏览器里。全部可以改。');
  console_.append(body(h('div', {},
    h('div', { class: 'choice-row' }, h('div', { class: 'label' }, '语言'),
      h('div', { class: 'choices' }, h('button', { type: 'button', class: 'choice', 'aria-pressed': 'true' }, '简体中文')),
      h('p', { class: 'small muted' }, '现在只有简体中文。别的语言还没有语言包。')),
    choiceRow('主题', 'theme', ctx, update, current),
    choiceRow('字体', 'font', ctx, update, current),
    choiceRow('密度', 'density', ctx, update, current),
    choiceRow('动效', 'motion', ctx, update, current),
    h('div', { class: 'choice-row' }, h('div', { class: 'label' }, '文字大小'), h('div', { class: 'row' }, size, sizeText)),
    h('div', { class: 'choice-row' }, h('div', { class: 'label' }, '强调色'), accents, contrastLine()),
    h('div', { class: 'choice-row' }, h('div', { class: 'label' }, '预览'),
      h('div', { class: 'sample' }, h('strong', {}, '电量 67%，充电中'), h('span', { class: 'muted' }, '这是示例文字，用来看字体、大小和颜色。'))))));

  const device = getDevice();
  const themeId = device?.reported_configuration?.layout?.theme;
  const themeName = THEMES.find(theme => theme.id === themeId)?.name;
  const deviceBox = panel(device ? `设备 ${device.device_id}` : '设备', '受字库和内存限制');
  const fact = (label, mark, value, note) => h('div', { class: 'kv' }, h('div', {}, h('strong', {}, label), h('div', { class: 'small muted' }, note)), h('div', {}, mark, ' ', value));
  deviceBox.append(body(h('div', {},
    fact('主题与配色', h('span', { class: 'chip ok' }, '可改'), themeName || '未上报', '在“布局与显示”里选薄荷夜色、深海蓝或暖橙，然后下发。'),
    fact('语言、文字大小、电池百分比、圆环粗细', h('span', { class: 'chip' }, '在设备上改'), '设置 → 外观', '设备不上报这几项，网页不显示也不改它们。'),
    fact('字体', h('span', { class: 'chip warn' }, '不可更换'), '思源黑体子集（28 / 20 px）', '内置；决策 D13：先不加数据分区。'),
    fact('恢复默认', h('span', { class: 'chip' }, '在设备上'), '长按按键 10 秒', '设备上的外观恢复为默认。')),
    h('div', {}, btn('去“布局与显示”', 'small', () => ctx.go('layout')))));

  const installed = panel('已安装的包');
  installed.append(body(h('div', {},
    fact('主题包', h('span', { class: 'chip' }, '设备内置'), '薄荷夜色 · 深海蓝 · 暖橙', '写在固件里。'),
    fact('布局包', h('span', { class: 'chip' }, '设备内置'), '桌面 · 时钟 · 专注', '主页的三种预设。'),
    fact('管理台外观', h('span', { class: 'chip ok' }, '可导入导出'), 'deskhub-appearance.json', '左边的选择，一个很小的 JSON 文件。'))));

  container.append(h('div', { class: 'page' },
    pageHead('外观', '管理台的语言、字体、文字大小、主题与配色。设备的外观受字库和内存限制，单独说明。',
      importInput,
      btn('导入', 'small', () => importInput.click(), { id: 'import-button' }),
      btn('导出', 'small', exportFile, { id: 'export-button' }),
      btn('恢复默认', 'small', () => { update({ ...DEFAULTS }); toast('已恢复默认外观'); }, { id: 'reset-button' })),
    h('div', { class: 'cols' }, h('div', { class: 'grow' }, console_), h('div', { class: 'side stack' }, deviceBox, installed))));
}
