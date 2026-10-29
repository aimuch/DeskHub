// 格式化：缺失的数据一律显示「未上报」，不当作 0。
export const NOT_REPORTED = '未上报';

export const describe = value => (value === null || value === undefined || value === '' ? NOT_REPORTED : String(value));

export function shortTime(value) {
  if (!value) return NOT_REPORTED;
  const time = new Date(value);
  if (Number.isNaN(time.getTime())) return '时间不可用';
  return time.toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
}

export function clockTime(value) {
  const time = value instanceof Date ? value : new Date(value);
  return Number.isNaN(time.getTime()) ? NOT_REPORTED : time.toLocaleTimeString('zh-CN', { hour12: false });
}

export function bytes(value) {
  const number = Number(value);
  if (value === null || value === undefined || !Number.isFinite(number)) return NOT_REPORTED;
  if (number >= 1048576) return `${(number / 1048576).toFixed(2)} MiB`;
  return `${Math.round(number / 1024)} KiB`;
}

export function rate(value) {
  const number = Number(value);
  if (value === null || value === undefined || !Number.isFinite(number)) return NOT_REPORTED;
  if (number >= 1048576) return `${(number / 1048576).toFixed(1)} MiB/s`;
  if (number >= 1024) return `${(number / 1024).toFixed(0)} KiB/s`;
  return `${Math.round(number)} B/s`;
}

export function duration(seconds) {
  if (!Number.isFinite(seconds)) return NOT_REPORTED;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 1) return '不足 1 分钟';
  if (minutes < 60) return `${minutes} 分钟`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours} 小时 ${minutes % 60} 分钟`;
  return `${Math.floor(hours / 24)} 天 ${hours % 24} 小时`;
}

export const clone = value => JSON.parse(JSON.stringify(value));

/** 距现在多久：用于“3 秒前同步”。时间不可读时写“未上报”。 */
export function ago(value, now = Date.now()) {
  const time = new Date(value).getTime();
  if (!value || Number.isNaN(time)) return NOT_REPORTED;
  const seconds = Math.max(0, Math.round((now - time) / 1000));
  if (seconds < 60) return `${seconds} 秒前`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
  if (seconds < 172800) return `${Math.floor(seconds / 3600)} 小时前`;
  return `${Math.floor(seconds / 86400)} 天前`;
}

export const volts = millivolts => (Number.isFinite(millivolts) ? `${(millivolts / 1000).toFixed(2)} V` : NOT_REPORTED);
export const celsius = value => (Number.isFinite(value) ? `${value.toFixed(1)} °C` : NOT_REPORTED);
