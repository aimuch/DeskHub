// 管理台的外观选择。只保存在这个浏览器里，只有下面这几项，值必须在固定的清单里。
// 这里不保存任何登录信息。
const KEY = 'deskhub.appearance';
const VERSION = 1;

export const CHOICES = {
  theme: [['dark', '深色'], ['light', '浅色'], ['system', '跟随系统']],
  accent: [['mint', '薄荷'], ['sky', '天蓝'], ['orange', '橙'], ['violet', '紫'], ['rose', '玫红'], ['lime', '青柠']],
  font: [['system', '系统默认'], ['serif', '衬线'], ['mono', '等宽']],
  density: [['compact', '紧凑'], ['standard', '标准'], ['relaxed', '宽松']],
  motion: [['on', '开'], ['reduce', '减少'], ['off', '关']],
};
export const SIZES = { min: 12, max: 20 };
export const DEFAULTS = { theme: 'system', accent: 'mint', font: 'system', density: 'standard', motion: 'on', size: 15 };

const allowed = (name, value) => CHOICES[name].some(([id]) => id === value);
const sizeOk = value => Number.isInteger(value) && value >= SIZES.min && value <= SIZES.max;

/** 只接受清单里的值；多余的项和不合法的值一律丢掉。 */
export function clean(source) {
  const result = { ...DEFAULTS };
  if (!source || typeof source !== 'object') return result;
  for (const name of Object.keys(CHOICES)) if (allowed(name, source[name])) result[name] = source[name];
  if (sizeOk(source.size)) result.size = source.size;
  return result;
}

export function load() {
  try {
    return clean(JSON.parse(window.localStorage.getItem(KEY)));
  } catch {
    return { ...DEFAULTS };
  }
}

export function save(value) {
  const prefs = clean(value);
  try {
    window.localStorage.setItem(KEY, JSON.stringify(prefs));
  } catch { /* 浏览器不让保存时，这次选择只在当前页面有效 */ }
  return prefs;
}

/** 写在 <html> 上；样式表按这些属性换颜色、字号和密度。 */
export function apply(value) {
  const prefs = clean(value);
  const root = document.documentElement;
  for (const name of Object.keys(CHOICES)) root.dataset[name] = prefs[name];
  root.dataset.size = String(prefs.size);
  return prefs;
}

export function exportText(value) {
  return JSON.stringify({ deskhub_appearance: VERSION, ...clean(value) }, null, 2);
}

/** 导入文件。版本不对、不是 JSON、有不认识的项或值不在清单里，都返回 null，不改动现有选择。 */
export function parseImport(text) {
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    return null;
  }
  if (!data || typeof data !== 'object' || Array.isArray(data) || data.deskhub_appearance !== VERSION) return null;
  const known = new Set(['deskhub_appearance', 'size', ...Object.keys(CHOICES)]);
  if (Object.keys(data).some(key => !known.has(key))) return null;
  for (const name of Object.keys(CHOICES)) if (!allowed(name, data[name])) return null;
  return sizeOk(data.size) ? clean(data) : null;
}

/** 文字对比度（WCAG）。颜色写成 #rrggbb 或 rgb(r, g, b)。 */
export function contrast(foreground, background) {
  const light = color => {
    const channels = (color.startsWith('#')
      ? [1, 3, 5].map(at => parseInt(color.slice(at, at + 2), 16))
      : color.match(/\d+(\.\d+)?/g).slice(0, 3).map(Number)).map(value => {
      const part = value / 255;
      return part <= 0.03928 ? part / 12.92 : ((part + 0.055) / 1.055) ** 2.4;
    });
    return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
  };
  const [bright, dark] = [light(foreground), light(background)].sort((a, b) => b - a);
  return (bright + 0.05) / (dark + 0.05);
}
