// 管理 API 客户端。会话只存在于 HttpOnly Cookie；CSRF 令牌只保存在内存。
const BASE = '/api/v1/admin';

export class ApiError extends Error {
  constructor(code, status) {
    super(code);
    this.code = code;
    this.status = status;
  }
}

export function createApi({ getCsrf, onUnauthorized }) {
  async function request(path, { method = 'GET', json, headers = {}, body } = {}) {
    const init = { method, headers: new Headers(headers), credentials: 'same-origin', cache: 'no-store', redirect: 'error' };
    if (json !== undefined) {
      init.headers.set('Content-Type', 'application/json');
      init.body = JSON.stringify(json);
    } else if (body !== undefined) {
      init.body = body;
    }
    // 两个登录入口还没有会话，不能带 CSRF；其余写操作（含设置密码、退出）必须带。
    if (method !== 'GET' && !(method === 'POST' && (path === '/session' || path === '/login'))) init.headers.set('X-CSRF-Token', getCsrf());
    let response;
    try {
      response = await fetch(BASE + path, init);
    } catch {
      throw new ApiError('NETWORK_UNAVAILABLE', 0);
    }
    let data = {};
    try {
      data = await response.json();
    } catch {
      if (response.ok) throw new ApiError('INVALID_RESPONSE', response.status);
    }
    if (!response.ok) {
      const error = new ApiError(data?.error?.code || 'REQUEST_FAILED', response.status);
      // 登录入口自己的 401 是“凭据不对”，不是“会话过期”。
      if (response.status === 401 && path !== '/session' && path !== '/login' && error.code === 'UNAUTHORIZED') onUnauthorized();
      throw error;
    }
    return data;
  }

  /** 上传带进度；固件包最大 7 MiB。 */
  function upload(path, file, { onProgress, onSent } = {}) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', BASE + path);
      xhr.withCredentials = true;
      xhr.timeout = 60000;
      xhr.setRequestHeader('Content-Type', 'application/zip');
      xhr.setRequestHeader('X-CSRF-Token', getCsrf());
      xhr.upload.addEventListener('progress', event => event.lengthComputable && onProgress?.(event.loaded / event.total));
      xhr.upload.addEventListener('load', () => onSent?.());
      xhr.addEventListener('load', () => {
        let data;
        try { data = JSON.parse(xhr.responseText); } catch { reject(new ApiError('INVALID_RESPONSE', xhr.status)); return; }
        if (xhr.status >= 200 && xhr.status < 300) resolve(data);
        else {
          if (xhr.status === 401) onUnauthorized();
          reject(new ApiError(data?.error?.code || 'UPLOAD_FAILED', xhr.status));
        }
      });
      xhr.addEventListener('error', () => reject(new ApiError('NETWORK_UNAVAILABLE', 0)));
      xhr.addEventListener('timeout', () => reject(new ApiError('UPLOAD_TIMEOUT', 0)));
      xhr.send(file);
    });
  }

  return { request, upload };
}
