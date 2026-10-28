# DeskHub 协议 v1.0

`openapi.json` 和 `schemas/` 从 Agent 的实际 FastAPI / Pydantic 实现生成。执行：

```sh
.venv/bin/python protocol/export_openapi.py
.venv/bin/python protocol/export_openapi.py --check
```

导出只创建临时数据库和未启动的应用，不监听端口、不调用显示器、不发放凭据。检查会拒绝与代码不一致的契约，并用实际模型验证 `examples/`。样例仅说明结构，不是真实硬件的执行记录；样例时间已固定，直接发送会因过期而拒绝。

HTTP 业务接口位于 `/api/v1`，采用 `Authorization: Bearer <每设备凭据>`。`read:status`、`control:displays`、`admin:config` 是独立权限；命令查询还校验设备身份和目标权限。仅本机 `/healthz` 无需凭据。局域网必须使用已配置的 TLS 与网络范围。

动作请求最多 4 KiB，快照最多 16 KiB。命令在规格字段基础上增加必填 `sent_at`，与服务端时间偏差最多 5 秒；`expires_at` 默认 `sent_at + 10 秒`，最长 30 秒。不能仅凭一个远期有效期推断客户端时间正确。重试必须保留原 `request_id`、幂等键、时间与完整载荷；相同设备与键的相同载荷返回同一命令，不同载荷冲突。丢失提交响应可通过 `GET /api/v1/commands?idempotency_key=...` 查询。

`queued` / `executing` 是过程状态。`succeeded` 必须具备匹配的读回证据；程序退出码 0 本身仅能说明程序阶段完成。切走后无法读回返回 `unconfirmed`。Agent 重启会将已开始但未记录结果的动作标为未确认，排队动作取消，不自动补执行。

USB 使用 ESP32-S3 原生 USB Serial/JTAG，UTF-8 JSON Lines，每条帧连同换行最多 16 KiB；响应为 `{ "ok": true, "result": {...} }` 或 `{ "ok": false, "error": {"code": "...", "message": "..."} }`。支持 `identify`、`status`、`set_time`、`provision`。`provision` 先持久提交，再返回 `result.saved: true`；工具只有收到明确保存确认才显示成功。证书通过 USB 导入，仍验证服务端证书链、主机名和有效期；未提供关闭校验的选项。

USB 证书最多 8 KiB，SSID 最多 32 UTF-8 字节，Wi-Fi 密码为空或 8–63 UTF-8 字节，Token 为 32–256 个可打印 ASCII 字符。JSON Schema 的长度约束以字符为单位，固件和配置工具还检查实际 UTF-8 字节数、PEM 解析、日历与 HTTPS 根地址语义。时间格式为 `YYYY-MM-DDTHH:MM:SSZ`，范围 2024–2099 年。私有配置信息不作为命令行参数或普通日志输出。

`provision` 的 `config.time.ntp_server` 可选，默认 `pool.ntp.org`；可使用局域网 NTP 主机名或 IPv4 地址，禁止 URL、路径和端口。工具也支持 `--ntp-server ntp.home.local`。单独 `set_time` 只设时，不改变已保存 NTP 地址。
