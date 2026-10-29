// 极小的 DOM 构造工具。所有文本都经 textContent / 文本节点写入，不使用 innerHTML。
const SVG_NS = 'http://www.w3.org/2000/svg';
const BOOLEAN_PROPS = new Set(['checked', 'disabled', 'hidden', 'selected', 'required', 'multiple', 'open']);

export const $ = id => document.getElementById(id);

export function append(parent, children) {
  for (const child of [children].flat(Infinity)) {
    if (child === undefined || child === null || child === false) continue;
    parent.append(typeof child === 'object' ? child : document.createTextNode(String(child)));
  }
  return parent;
}

/** h('div', {class: 'x', onclick}, ...children) */
export function h(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  let value;
  for (const [key, prop] of Object.entries(props || {})) {
    if (prop === undefined || prop === null || prop === false) continue;
    if (key === 'class') node.className = prop;
    else if (key === 'value') value = prop; // select 需等 option 就位后再赋值
    else if (key === 'dataset') Object.assign(node.dataset, prop);
    else if (key === 'for') node.htmlFor = prop;
    else if (key.startsWith('on') && typeof prop === 'function') node.addEventListener(key.slice(2), prop);
    else if (BOOLEAN_PROPS.has(key)) node[key] = true;
    else node.setAttribute(key, prop === true ? '' : String(prop));
  }
  append(node, children);
  if (value !== undefined) node.value = value;
  return node;
}

const ICONS = {
  overview: 'M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z',
  devices: 'M4 5h16v11H4zM9 20h6M12 16v4',
  actions: 'M13 3 5 13h6l-1 8 8-10h-6z',
  commands: 'M8 6h12M8 12h12M8 18h12M4 6h.01M4 12h.01M4 18h.01',
  layout: 'M4 4h16v16H4zM4 10h16M12 10v10',
  firmware: 'M12 16V4M7 9l5-5 5 5M4 15v4a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-4',
  access: 'M15 5a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 12 4 20M7 17l2 2',
  power: 'M13 2 4 14h7l-1 8 9-12h-7z',
  appearance: 'M12 3a9 9 0 1 0 0 18c1.5 0 2-1 1.5-2.2-.6-1.3.2-2.8 1.7-2.8H18a3 3 0 0 0 3-3A9 9 0 0 0 12 3zM7.5 11h.01M10 7.5h.01M14.5 7.5h.01',
  refresh: 'M20 7v5h-5M4 17v-5h5M6.1 7a7 7 0 0 1 11.6-2L20 8M4 16l2.3 3A7 7 0 0 0 18 17',
  check: 'M5 12l5 5 9-10',
  alert: 'M12 4 3 20h18zM12 10v4M12 17h.01',
  info: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v5M12 8h.01',
  copy: 'M9 9h10v11H9zM5 15V4h10',
};

export function icon(name) {
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', 'icon');
  const path = document.createElementNS(SVG_NS, 'path');
  path.setAttribute('d', ICONS[name] || ICONS.info);
  svg.append(path);
  return svg;
}

/** SVG 元素。属性一律用 setAttribute，不用内联样式（CSP）。 */
export function svg(tag, attrs = {}, ...children) {
  const node = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs)) if (value !== undefined && value !== null) node.setAttribute(key, String(value));
  append(node, children);
  return node;
}
