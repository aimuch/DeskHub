// 界面文案。原则：说清发生了什么、下一步做什么；技术码放在次要位置。

export const ERRORS = {
  NETWORK_UNAVAILABLE: '无法连接 Agent。请确认它仍在运行。',
  INVALID_RESPONSE: 'Agent 返回了无法识别的内容。',
  UNAUTHORIZED: '登录已过期，请重新连接管理台。',
  FORBIDDEN: '当前凭据没有执行此操作的权限。',
  ORIGIN_FORBIDDEN: '请求来源与管理台不一致。请从 Agent 的管理台地址打开页面。',
  CSRF_REJECTED: '会话校验失败，请重新登录后再试。',
  RATE_LIMITED: '请求较频繁，请稍后重试。',
  INVALID_ARGUMENT: '输入的值无效，请检查后重试。',
  PAYLOAD_TOO_LARGE: '上传内容过大，请选择不超过 7 MiB 的签名固件包。',
  DEVICE_NOT_FOUND: '设备不存在，请刷新设备列表。',
  DEVICE_REVOKED: '设备凭据已吊销，需要通过 USB 重新配对后再修改配置。',
  DEVICE_OFFLINE: '设备当前离线，请等它重新上报后再升级。',
  UNSUPPORTED: '设备不支持这一操作。',
  REVISION_CONFLICT: '设备配置已被其他操作修改，请重新载入后再保存。',
  BUSY: '设备已有进行中的升级，请等它结束。',
  ALREADY_INSTALLED: '设备已经安装这个版本，无需重复升级。',
  FIRMWARE_REVISION_CONFLICT: '设备版本已改变，请刷新后重新选择升级。',
  FIRMWARE_VERSION_MISMATCH: '设备版本已改变，请刷新后重新选择升级。',
  UNSAFE_UPDATE_POWER: '需要最近上报的、无故障的 USB 供电。',
  UPDATE_TRUST_MISMATCH: '设备、Agent 与固件包的升级公钥不一致，请用 USB 核对设备信任。',
  UPDATE_TRUST_NOT_CONFIGURED: 'Agent 尚未配置升级公钥，请先在本机完成设置。',
  UPDATE_TRUST_INVALID: 'Agent 配置的升级公钥无效，请检查本机公钥文件。',
  UPDATE_JOB_NOT_FOUND: '找不到对应的升级任务。',
  FIRMWARE_NOT_FOUND: '找不到这个固件包。',
  FIRMWARE_CAPACITY_REACHED: '固件库已满（最多 20 个）。请先在本机清理不再需要的包。',
  FIRMWARE_TRUST_CONFLICT: '这个镜像已用另一把签名密钥入库，不能重复使用。',
  FIRMWARE_SIGNATURE_INVALID: '固件签名与 Agent 信任的公钥不符，未入库。',
  FIRMWARE_HASH_MISMATCH: '固件镜像校验不一致，未入库。',
  INVALID_FIRMWARE_PACKAGE: '升级包格式无效。请上传仅含 app.bin 与 manifest.json 的 .dhfw 包；USB 刷机包不能用于升级。',
  INVALID_FIRMWARE_MANIFEST: '固件签名清单格式无效，未入库。',
  INVALID_FIRMWARE_IMAGE: '这不是有效的 ESP-IDF 应用镜像。',
  WRONG_CHIP: '固件芯片型号与 ESP32-S3 不一致。',
  WRONG_APPLICATION: '固件不是 DeskHub 应用，未入库。',
  UPLOAD_TIMEOUT: '上传超时，请重试。',
  UPLOAD_FAILED: '上传失败，请重试。',
  LOGIN_FAILED: '密码不对。',
  ACCOUNT_LOCKED: '输错太多次，网页登录已锁定 15 分钟。到时间会自动解开。',
  TOKEN_LOGIN_DISABLED: '已经设置过密码，网页不再接受口令。请用密码登录。',
  PASSWORD_REQUIRED: '请先设置管理员密码。',
  PASSWORD_ALREADY_SET: '密码已经设置过了。请用密码登录。',
  WEAK_PASSWORD: '这个密码太弱：至少 12 个字符，不要用常见的词、重复或连续的字符，也不能和口令相同。',
  CURRENT_PASSWORD_REQUIRED: '修改密码要先输入当前密码。',
  AUTH_UNAVAILABLE: '登录服务暂时不可用，这次登录已拒绝。请稍后再试。',
  PAIRING_NOT_CONFIGURED: '无线配对还没配置：Agent 配置里要有 public_url 和 pairing_ca。',
  TOO_MANY_GRANTS: '等待中的授权太多。取消一个，或等它过期。',
  GRANT_NOT_FOUND: '找不到这个授权。',
  GRANT_NOT_PENDING: '只有还在等待的授权可以取消。',
  PAIRING_UNAVAILABLE: '无线配对暂时不可用。',
};

export const errorText = error => ERRORS[error?.code] || '操作未完成。请稍后重试，或查看错误码。';

export const UPDATE_STATES = {
  queued: '等待设备接收', downloading: '正在下载', verifying: '正在验证', writing: '正在写入', rebooting: '正在重启',
  pending_confirmation: '等待首启自检', succeeded: '升级已确认', failed: '升级失败', rolled_back: '已回滚', unconfirmed: '结果未确认',
};
export const ACTIVE_UPDATE = new Set(['queued', 'downloading', 'verifying', 'writing', 'rebooting', 'pending_confirmation']);
export const UPDATE_STAGES = [['downloading', '下载'], ['verifying', '验证'], ['writing', '写入'], ['rebooting', '重启'], ['pending_confirmation', '自检']];

export const CONFIG_STATES = {
  pending: ['等待设备确认', 'warn'], applied: ['设备已应用', 'ok'], failed: ['应用失败', 'bad'], unavailable: ['尚未上报', 'neutral'],
};

export const COMMAND_STATES = {
  queued: ['排队中', 'neutral'], executing: ['执行中', 'neutral'], succeeded: ['已确认', 'ok'], unconfirmed: ['未确认', 'warn'],
  failed: ['失败', 'bad'], expired: ['已过期', 'neutral'], cancelled: ['已取消', 'neutral'], partial: ['部分完成', 'warn'],
};

// 命令结果的原因码。没有证据就不写「成功」。
export const REASONS = {
  READBACK_UNSUPPORTED: '显示器未启用读回，命令已发送但无法确认',
  VERIFICATION_UNAVAILABLE: '无法读回当前输入源，命令已发送但无法确认',
  READBACK_MISMATCH: '读回的输入源与目标不一致',
  CONTROL_PATH_LOST_AFTER_SWITCH: '切换后控制通道丢失，无法读回',
  AGENT_RESTARTED_DURING_EXECUTION: 'Agent 在执行期间重启，结果未知，不会自动重试',
  AGENT_RESTARTED_BEFORE_EXECUTION: 'Agent 在执行前重启，命令未下发',
  AGENT_STOPPING: 'Agent 正在停止',
  COMMAND_EXPIRED: '超过有效期，未下发',
  BACKEND_TIMEOUT: '显示器后端超时',
  BACKEND_REJECTED: '显示器后端拒绝了命令',
  BACKEND_OUTPUT_INVALID: '显示器后端返回了无法识别的内容',
  TARGET_UNAVAILABLE: '显示器当前不可用',
  DISPLAY_BINDING_MISSING_OR_AMBIGUOUS: '找不到绑定的显示器，或匹配到多台',
  DISPLAY_BINDING_CHANGED_RECONFIRM_REQUIRED: '显示器绑定已变化，需要在本机重新确认',
  DISPLAY_SERIAL_AMBIGUOUS_RECONFIRM_REQUIRED: '多台显示器序列号相同，需要在本机重新确认',
  INTERNAL_ERROR: 'Agent 内部错误，结果无法确定',
};
export const reasonText = code => (code ? REASONS[code] || code : '');

export const BACKEND_REASONS = {
  BACKEND_NOT_CONFIGURED: '未配置显示器后端',
  PLATFORM_UNSUPPORTED: '当前系统不支持这个后端',
  BACKEND_NOT_INSTALLED: '找不到后端可执行文件',
  BINDING_NOT_YET_CHECKED: '尚未检查显示器绑定',
  READBACK_DISABLED: '未启用读回',
  SNAPSHOT_STALE: '状态已过期',
  USER_CONFIGURED_MAPPING_ONLY: '仅使用本机配置的输入映射',
  ...REASONS,
};
export const backendReason = code => (code ? BACKEND_REASONS[code] || code : '');

export const METRIC_REASONS = {
  SAMPLER_WARMUP: '正在采样预热',
  SAMPLER_WARMUP_OR_COUNTER_RESET: '正在采样预热',
  SENSOR_UNAVAILABLE: '传感器不可用',
  CPU_TEMPERATURE_NOT_IMPLEMENTED: '不支持',
};

export const THEMES = [
  { id: 'mint', name: '薄荷夜色', description: '清爽、安静' },
  { id: 'ocean', name: '深海蓝', description: '冷静、专注' },
  { id: 'amber', name: '暖橙', description: '柔和、温暖' },
];
export const PRESETS = [['desk', '桌面'], ['clock', '时钟'], ['focus', '专注']];
export const ROTATIONS = [0, 90, 180, 270];
