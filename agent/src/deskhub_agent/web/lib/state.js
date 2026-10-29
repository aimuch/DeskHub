// 管理台的内存状态。不使用任何浏览器持久存储。
import { ACTIVE_UPDATE } from './copy.js';

export const state = {
  session: null,
  csrf: '',
  page: 'overview',
  overview: null,
  devices: [],
  packages: null, // null = 尚未读取；与“固件库为空”不同
  host: null,
  displays: null,
  commands: null,
  selectedId: '',
  detail: null,
  draft: null,
  draftRevision: 0,
  dirty: false,
  busy: false,
  lastRefresh: null,
};

export const activeDevices = () => state.devices.filter(device => !device.revoked);
export const isOnline = device => Boolean(device?.online && !device?.revoked);
export const getDevice = () => (state.detail?.device_id === state.selectedId ? state.detail : state.devices.find(device => device.device_id === state.selectedId));
export const deviceActions = device => (device && !device.revoked ? device.actions || [] : []);
export const hasActiveUpdate = device => ACTIVE_UPDATE.has(device?.update?.state);
export const canManage = device => Boolean(device && !device.revoked && device.capabilities?.management);

export function resetSession() {
  Object.assign(state, {
    session: null, csrf: '', overview: null, devices: [], packages: null, host: null, displays: null, commands: null,
    selectedId: '', detail: null, draft: null, draftRevision: 0, dirty: false, busy: false, lastRefresh: null,
  });
}
