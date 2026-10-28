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

`device-report` 的 `power` 对象：`vbus`、`battery`、`charging`、`fault` 沿用旧含义。固件 0.3.0 起另有 `soc_percent`（电源芯片自己的电量估算，未对照电芯验证）、`battery_mv`、`vbus_mv`、`chip_temp_c`（电源芯片的温度，不是电池温度）和 `measurement`（`ok`、`adc_enable_failed`、`adc_read_failed`、`adc_out_of_range`、`not_measured`）。旧固件不带这些字段，Agent 按“未测量”处理。

数值必须在合理范围内（电池 2500–4600 mV，USB 3000–6500 mV，芯片温度 −40 到 150 °C，电量 0–100），超出范围的整份上报被拒绝，Agent 不截断、不保存。测量失败时不能带电压和温度；没有电池时不能带电量和电池电压；没有 USB 时不能带 USB 电压。管理接口的设备记录另有 `power_state`（`unreported`、`fresh`、`stale`）和 `power_reported_at`：只有设备在线（最近 30 秒有上报）时才是 `fresh`；`stale` 时数值全部为空，只保留最后一次上报的时间。

热点配网的 Agent 接口见 `docs/pairing-design.md`。管理员接口（需要管理员会话、CSRF 和同源）：`POST /api/v1/admin/pairing-grants` 生成授权，响应里的 `package` 只出现这一次；`GET` 列出授权的状态和剩余尝试次数，不含任何秘密；`DELETE /api/v1/admin/pairing-grants/{grant_id}` 取消。设备接口 `POST /api/v1/pairing/redeem` 不需要凭据，只接受 HTTPS：请求带 `grant_id`、`code`、`device_id`，成功返回 `agent_id`、`token`、`ca_pem`、`scopes`、`targets`。授权编号不存在、授权码错、过期、已用、已取消、已烧毁、设备编号与授权指定的不符，都返回同一个 403 `PAIRING_REJECTED`。同一来源每分钟最多 10 次，全部来源每分钟最多 30 次；限流器自己出错时返回 503，不放行。

配对包 `DHP2.…`（版本 2）带着 Agent 的 CA 证书（DER，最多 2048 字节，恰好是一个 DER 序列）。它的格式和测试向量（4 个合格、22 个不合格）在 `examples/pairing-package.json`，Agent 的解码器和设备的实现都要通过它们。`DHP1.…`（只带 CA 指纹）已经取消，两边的解码器都拒绝它。Agent 配置新增 `public_url`（设备连 Agent 的 `https://主机[:端口]`）和 `pairing_ca`（CA 证书文件，只含公开证书）；缺任一项，或 CA 文件不是证书，或 CA 的 DER 超过 2048 字节，生成授权返回 409。生成授权的响应里 `ca_fingerprint` 是 CA 的 SHA-256（十六进制），只给人核对，设备不用它。

没有 CA 下载接口：设备把配对包里的 CA 当作唯一信任根，从第一个字节起就建立验证过的 TLS 连接，所以没有任何未验证的连接。兑换响应里的 `ca_pem` 必须和配对包里的 CA 是同一份（DER 逐字节相同），否则设备中止。
