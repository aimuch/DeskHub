# DeskHub Agent v0.2 开发版

默认仅监听 `127.0.0.1:8765`，业务 API 需要独立设备 Token。当前实现 macOS BetterDisplay CLI 后端；没有真实显示器切出/切回证据。未配置后端时指标与认证服务仍可使用，显示器动作返回不可用。没有生产模拟后端。

## 安装与启动

从仓库根目录（Python 3.12–3.14，开发验证使用 3.13.18）：

```sh
uv venv --python 3.13 .venv
UV_PROJECT_ENVIRONMENT=../.venv uv sync --project agent --frozen --extra test
.venv/bin/deskhubctl --config .local/agent.json init
.venv/bin/deskhubctl --config .local/agent.json serve
```

前台服务使用 Ctrl-C 停止。不要求 root；同一数据库只能启动一个 Agent 进程。`serve` 禁用代理头与访问日志，不接受通过命令行传 Token。业务日志位于数据库旁的 `agent.log`，每份最多 1 MiB，保留三份轮转。

配置路径显式传给每条命令；未传则使用 `~/.config/deskhub/agent.json`。相对数据库、证书和密钥路径均相对配置文件解析。`init` 生成稳定随机 agent_id，不覆盖已有文件。

所有直接依赖由 `pyproject.toml` 精确固定，传递依赖由 `uv.lock` 固定。测试执行：

```sh
.venv/bin/python -m pytest agent/tests -q
```

## 配对与权限

```sh
.venv/bin/deskhubctl --config .local/agent.json pair desktop-cli --target monitor-desk-01 --output .local/desktop-credential.json
.venv/bin/deskhubctl --config .local/agent.json devices
.venv/bin/deskhubctl --config .local/agent.json revoke desktop-cli
```

Token 由 `secrets.token_urlsafe(32)` 生成；SQLite 只存 SHA-256，文件权限 0600。首次明文仅写私有凭据文件，不打印日志。权限为 `read:status`、`control:displays`、`device:sync` 和独立的管理员权限 `admin:config`。通过 `--scope` 限定。建议逐个指定 `--target`；未指定时 CLI 默认授权所有已配置目标。每台终端独立配对；已配对设备默认拒绝覆盖，`--replace` 明确轮换。USB 写入失败会吊销新凭据，需要重新配对。

## 网页管理台

> main 分支的网页管理台是 2026-10-29 重做的（九个页面，登录要先设置密码，见 [docs/web-console-design.md](../docs/web-console-design.md) 和 [docs/admin-login-design.md](../docs/admin-login-design.md)）。下面这一节是从旧版带来的说明，登录的部分已经过时：第一次用口令登录以后必须设置密码，之后网页只接受密码。

启动 Agent 后打开 [本机管理台](http://127.0.0.1:8765/manage/)。管理台与同一 Agent 部署，不需要云服务或额外进程。创建单独的管理凭据：

```sh
.venv/bin/deskhubctl --config .state/agent.json admin-token --output .state/admin-credentials.json
```

打开这个 0600 私有文件，把 token 复制到管理台登录框。不要放入 URL、命令参数或 localStorage。管理会话使用 HttpOnly / SameSite=Strict cookie，有效期 30 分钟，HTTPS 时设置 Secure；管理员写请求还需要同源 Origin 与独立 CSRF token。`revoke browser-admin` 立即使原会话失效，`admin-token --replace --output NEW_FILE` 轮换凭据。

管理台提供真实已配对终端列表、最近上报、设备吊销、显示设置与 desk/clock/focus 预设布局、动作卡片顺序、签名固件上传与更新进度，以及只读的主机指标、显示器与动作配置、命令记录（接口见 [协议说明](../protocol/README.md)；显示器绑定标识不会通过网页返回）。没有设备上报时显示未上报/离线，不添加演示设备。配置保存先成为待下发版本，只有设备校验并持久化后的对应 revision / configuration 上报才显示已应用。固件本地修改造成差异时如实显示。

新固件每 10 秒通过 `device:sync` 身份报告状态，在线判断为最近 30 秒。Agent 重启后旧报告仍保留，但设备重新上报前保持离线。普通终端凭据不能访问管理 API，管理凭据也不能冒充终端同步或下载镜像。

签名升级使用独立的 P-256 更新根。`firmware_public_key` 指向本机受信 SPKI PEM 公钥文件（相对配置文件目录解析）；同一公钥须事先通过 USB 安装到终端。私钥不交给管理网页、Agent 服务或设备。签名工具与初次配置步骤见项目根目录文档。上传包最大 7 MiB，ZIP 仅包含 app.bin 和 manifest.json；验证签名、板卡、芯片、应用名称/版本、大小、hash 和 schema 后才入库。包内公钥不会成为信任根。

发起无线更新要求设备最近在线、USB 供电无故障、设备已启用签名更新并且公钥指纹匹配。排队任务 5 分钟内未开始即失效；一台终端同时最多一个活动更新。下载或写入完成不能变成升级成功，只有新固件报告首启确认与目标版本一致才记为 succeeded。进行中的设备凭据被吊销时结果保留未确认，不假称硬件撤销。原始镜像下载仅对收到该任务的终端开放，静态页面与设备下载分别有固定路径。

固件包初始最多保留 20 份；设备列表有分页以保持 16 KiB 快照上限。应用配置目前兼容 schema 1，不宣称任意版本迁移已完成。真实 USB / OTA 断电与回滚仍需硬件验收。

在凭据文件增加 `agent_url` 与 `ca_file`，然后：

```sh
.venv/bin/deskhubctl --config .local/agent.json status --credentials .local/desktop-credential.json
.venv/bin/deskhubctl --config .local/agent.json actions --credentials .local/desktop-credential.json
.venv/bin/deskhubctl --config .local/agent.json run desktop.switch_to_mac --credentials .local/desktop-credential.json --wait
```

`--url http://127.0.0.1:8765` 只适用于本机 CLI。局域网必须 HTTPS；配置 `lan_enabled=true`、明确 `bind`、证书/密钥及 `allowed_networks`。工具不禁用证书验证，也不跟随重定向传递凭据。

`run` 在网络发送前，把不可变原请求保存为凭据文件旁的 `.last-command.json`；响应丢失时用原幂等键查询。查询不到时报告失败，不生成新键盲目重试。`result COMMAND_ID --credentials ...` 查询最终结果；`recover --credentials ...` 用保存在本机的原幂等键恢复上次结果，不重新执行动作。`--wait` 最多等 15 秒，超时后仍返回 command_id 供继续查询。

## 显示器配置与 G0

仅支持显式配置的 BetterDisplay CLI 路径，不自动安装后端或改动显示器。必须先安装并在用户会话运行 BetterDisplay，启用其 CLI 集成。部分功能与许可证、版本有关。

配置 `backend="betterdisplay"`、`backend_path` 为实际可执行文件绝对路径，然后执行只读枚举：

```sh
.venv/bin/deskhubctl --config .local/agent.json list-displays
```

当前依据 [BetterDisplay 官方 CLI reference](https://betterdisplay.pro/guide/integration/cli-reference/) 实现：`get --identifiers`；每次动作前校验 UUID、非零 serial 与 registryLocation，缺失、重复序列号、路径改变均停止控制，要求人工重绑。不采用易变 displayID/列表索引。未识别的 JSON 结构会拒绝，不猜测绑定。

将 `examples/display-binding.json` 的字段按实测填入 `displays`，`inputs` 必须填入该实际型号验证的稳定 input_id 到 DDC 值的映射。模板故意保留空 inputs，不能直接当成有效配置。没有 USB-C/DP 默认输入值。`readback_enabled` 仅在确认直接 DDC 读取受支持后打开。`compatibility_verified`、`return_path_verified` 仅按实测证据标记。

动作配置示例（输入映射须先实测）：

```json
{
  "action_id": "desktop.switch_to_mac",
  "label": "切到 Mac",
  "target_id": "monitor-desk-01",
  "input_id": "mac-usbc",
  "enabled": true,
  "shortcut": null
}
```

设置使用直接 DDC `--feature=ddc --vcp=0x60 --value=已配置值`。执行后只有新的直接读回值匹配目标才是 succeeded；退出码 0、不可读回、切走后控制通道消失、异常退出均不能证明成功。系统保留 requested 与 observed 的区别。

该适配器代码与故障注入测试已建立，真实 CLI 输出格式/版本、监视器型号、扩展坞链路与切回仍须 G0 验证。输入枚举能力为 unsupported；显示器列表中的输入来自用户配置，不声称来源于硬件枚举。m1ddc、Windows/Linux 后端、网关未实现。

## 快捷键

同一 `deskhubctl run ACTION_ID` 命令可由 macOS Shortcuts、Raycast、Hammerspoon 等用户已有快捷键工具调用，仍经同一 HTTPS/本机 API、幂等日志和调度器。使用绝对可执行/配置/凭据路径。不要把 Token 写入快捷键命令参数。

`shortcut ACTION_ID --key 'ctrl+alt+m'` 保存外部热键绑定声明，检查 DeskHub 配置内冲突；`--disable` 清除。DeskHub 不自动注册全局热键，也无法检测其他程序占用的系统快捷键，需在所用工具内确认冲突。触屏/CLI/快捷键共享 action_id，不能绕过服务执行后端。

## macOS 自启动、停止与卸载

`packaging/com.deskhub.agent.plist.in` 为 LaunchAgent 模板。用户自行替换两个绝对路径占位符并复制到 `~/Library/LaunchAgents/com.deskhub.agent.plist`（本项目不自动修改该目录）。只在登录用户会话运行；不转为 root daemon。安装后可按 macOS `launchctl bootstrap gui/$(id -u) ...plist` 加载，`launchctl bootout gui/$(id -u) ...plist` 停止。卸载时先 bootout，再删除自己安装的 plist；按需保留或删除私有 `.local` 状态目录和虚拟环境。

## 恢复与诊断

`diagnostics` 输出版本、后端可用性与资源数量，不包含 Token、证书密钥或完整网络拓扑。数据库在 queued 后先持久化，再持久化 executing 边界后调用硬件；重启将旧 executing 标为 unconfirmed，旧 queued 标为 cancelled，不自动重放。幂等历史至少保留 24 小时。同一显示器活动命令返回 BUSY，执行前再次检查 UTC 与单调有效期。

调用者必须带 UTC sent_at；偏差超过 5 秒拒绝。expires_at 默认 sent_at+10 秒，上限 30 秒。NTP 调整不能延长已接收动作的单调截止时刻。此设计控制副作用风险，不承诺硬件与数据库跨进程严格 exactly-once。
