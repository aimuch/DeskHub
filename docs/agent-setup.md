# Agent 路线（可选）

DeskHub 最早的做法：Mac 上跑一个 Python 程序（Agent），触摸屏通过 Wi-Fi 和 HTTPS 连它，显示器用 BetterDisplay 切换。
现在推荐用桌面 App 加蓝牙（见 [README](../README.md)）：不用装 Python、不用证书、不用 BetterDisplay。
这条路线的代码还在，下面是原来 README 里的说明，没有改动内容。

## 你需要什么

- 一台 Mac（macOS）。
- Python 3.12 到 3.14，和 [uv](https://docs.astral.sh/uv/)。
- 一块 Waveshare ESP32-S3-Touch-AMOLED-2.16，一根能传数据的 USB-C 线。
- 一个 2.4 GHz 的 Wi-Fi，电脑和设备要在同一个局域网里。
- 要切换显示器输入：BetterDisplay（见“显示器切换”）。

## 快速开始

下面的命令都在仓库根目录运行。第 1 到 3 步的命令，和第 6 步里 Agent 一侧的命令，我在干净的临时目录里实际跑过（装、生成证书、配对、启动、查询）。
第 4、5 步需要真机，重做之后的版本没有完整走过，出问题请看“常见问题”。

### 1. 安装 Agent

```sh
uv venv --python 3.13 .venv
UV_PROJECT_ENVIRONMENT=$PWD/.venv uv sync --project agent --frozen
uv pip install --python .venv/bin/python -r tools/requirements.txt
```

第三行装的是 USB 工具要用的串口库。

### 2. 生成配置和证书

设备只信你自己生成的证书，所以先生成一个：

```sh
mkdir -p ~/.config/deskhub
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json init
.venv/bin/python tools/create_tls.py --out ~/.config/deskhub/tls \
    --dns 你的Mac主机名.local --ip 你的Mac局域网IP
```

`--dns` 和 `--ip` 可以重复写。**以后设备连 Agent 用的地址，必须出现在这里。** 地址对不上，连接会失败。

然后打开 `~/.config/deskhub/agent.json`，改这几项，其余不动：

```json
"bind": "你的Mac局域网IP",
"lan_enabled": true,
"tls_cert": "tls/server.pem",
"tls_key": "tls/server-key.pem",
"allowed_networks": ["127.0.0.0/8", "你的局域网网段，例如 192.168.1.0/24"]
```

局域网必须用 HTTPS，所以证书和私钥一起填。私钥只留在这台 Mac 上。

### 3. 启动 Agent

```sh
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json serve
```

它在前台运行，按 Ctrl-C 停止。macOS 可能会问你要不要允许它接收网络连接，选允许。
想让它开机自启，看 [agent/README.md](../agent/README.md) 的“macOS 自启动”。

### 4. 刷固件

要有固件包 `deskhub-<版本>-esp32s3.zip`。它由 `scripts/package_firmware.py` 生成，见 [开发笔记](development.md)。
解压后，在解压目录：

```sh
python3 -m venv .flash-venv
.flash-venv/bin/python -m pip install -r requirements-flash.txt
.flash-venv/bin/python flash.py --check
.flash-venv/bin/python flash.py --list
.flash-venv/bin/python flash.py --port /dev/cu.usbmodem……    # 换成 --list 显示的端口
```

`--check` 不碰设备。`flash.py` 不会擦掉设备上已有的配置。
端口找不到、写入失败，看 [docs/flashing.md](flashing.md)。

### 5. 用 USB 把设备连到电脑

先确认设备在线：

```sh
.venv/bin/python tools/provision.py --port /dev/cu.usbmodem…… --identify
```

再配对。配对会在 Agent 里给这台设备建一份独立的凭据，连同 Wi-Fi、Agent 地址和证书一起写进设备：

```sh
.venv/bin/python tools/provision.py --port /dev/cu.usbmodem…… \
    --agent-config ~/.config/deskhub/agent.json --ca ~/.config/deskhub/tls/ca.pem \
    --url https://你的Mac局域网IP:8765 --scope read:status --scope device:sync
```

工具会问 Wi-Fi 名称（只认 2.4 GHz）和密码。密码不会出现在命令行里，也不写入日志。
`--url` 里的地址要和第 2 步证书里的一致。

以后要用设备切换显示器，再配对一次，加上 `--scope control:displays --target 显示器编号`（见“显示器切换”）。
已经配对过的设备，要重新配对就加 `--replace`。

### 6. 检查

- 设备顶栏左上角显示“电脑在线”，说明设备连上了 Agent。刚开机要等十几秒。
- `.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json devices` 能看到这台设备。
- `.venv/bin/python tools/provision.py --port /dev/cu.usbmodem…… --status` 读设备的状态。刚开机十几秒内显示 `ui_alive: false` 是正常的。

## 日常使用（Agent）

### 网页管理台

`deskhubctl serve` 启动时会打印管理台的地址，形如 `https://你的Mac局域网IP:8765/manage/`。

1. **第一次**：用 `deskhubctl admin-token --output admin.json` 生成管理员口令，打开管理台，选“口令（首次）”，粘贴 `admin.json` 里的 `token`。
2. 登录后**必须马上设置密码**（至少 12 个字符，不能是常见的弱密码，也不能和口令相同）。设置完以前，其他页面都打不开。
3. 以后只用密码登录；网页不再接受口令，口令留给命令行和脚本。输错 5 次，网页登录锁 15 分钟。
4. **忘记密码**：在运行 Agent 的电脑上执行 `deskhubctl --config ~/.config/deskhub/agent.json admin-password-reset`。网页上没有“忘记密码”。
5. 页面：概览、设备、动作与显示器、命令记录、布局与显示、电源、固件升级、访问管理、外观。没有数据的地方写“未上报”，过期的数据写“数据已过期”。
6. 浏览器提示证书不受信任：见登录页右边的“信任证书”三步。

管理台的设计和限制见 [docs/web-console-design.md](web-console-design.md)。

### 在电脑上（命令行）

先给命令行单独配一份凭据：

```sh
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json pair desktop-cli --output ~/.config/deskhub/desktop-credential.json
```

打开这个文件，加两项：`"agent_url": "https://你的Mac局域网IP:8765"` 和 `"ca_file": "/Users/你/.config/deskhub/tls/ca.pem"`。然后：

```sh
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json status  --credentials ~/.config/deskhub/desktop-credential.json
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json actions --credentials ~/.config/deskhub/desktop-credential.json
.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json run desktop.switch_to_mac --credentials ~/.config/deskhub/desktop-credential.json --wait
```

触摸屏、命令行和快捷键（Shortcuts、Raycast 等）用同一套动作，同一个 `action_id`。

### 显示器切换

这一部分**没有真实显示器的证据**。它需要你自己实测：

1. 安装 BetterDisplay，打开它的命令行集成。
2. 在 `agent.json` 里设 `"backend": "betterdisplay"` 和 `"backend_path"`（可执行文件的绝对路径）。
3. 运行 `.venv/bin/deskhubctl --config ~/.config/deskhub/agent.json list-displays`，读出显示器的 UUID、序列号和位置。
4. 照 [agent/examples/display-binding.json](../agent/examples/display-binding.json) 把 `displays` 填好。`inputs` 要填你这台显示器真实的输入源数值，**没有默认值**。
5. 在 `actions` 里加动作（例子在 [agent/README.md](../agent/README.md)），重启 Agent，再给设备配对 `control:displays`。

细节和安全规则在 [agent/README.md](../agent/README.md)。

## 常见问题

**找不到设备的串口。** 先换一根能传数据的 USB 线，关掉别的串口工具。还不行：按住 BOOT，插 USB，松开 BOOT，再 `flash.py --list`。

**Agent 命令报 `ConnectError`。** 最常见的原因是地址和证书对不上：`--url` 或 `agent_url` 里的地址，要在第 2 步的 `--dns` 或 `--ip` 里。其次：Agent 没在运行；`allowed_networks` 没包含设备所在的网段；Mac 的防火墙挡住了。

**设备显示“电脑离线”。** 同上。另外确认设备和 Mac 在同一个 2.4 GHz 网络，Wi-Fi 密码没输错。设备会自动重连。

**想重新配对。** 再运行第 5 步的命令，加 `--replace`。`deskhubctl devices` 列出已配对的设备，`deskhubctl revoke 设备编号` 吊销一个。

**刷机以后设备一直不充电。** 你可能带着“停充”状态刷了旧固件。刷回新固件，它启动时会恢复充电。

**设备怎么找到电脑？** 靠配对时存进设备的 Agent 地址（`https://IP:8765`）；设备不会自己发现电脑。所以请给这台 Mac 在路由器里固定一个 IP。地址变了，设备就显示“电脑离线”，要重新配对。建议用 IP，不要用 `xxx.local` 这样的名字：固件里没有 mDNS，设备可能解析不了（没有在真机上试过）。

**服务器证书到期了怎么办？** 它只有 397 天。到期后设备会拒绝 Agent，显示“电脑离线”。到期前用同一个 CA 续期，设备和浏览器都不用动：

```sh
openssl x509 -enddate -noout -in ~/.config/deskhub/tls/server.pem        # 看哪天到期
.venv/bin/python tools/create_tls.py --ca-dir ~/.config/deskhub/tls --out ~/.config/deskhub/tls-new \
    --ip 你的Mac局域网IP --dns 你的Mac主机名.local                        # --ip、--dns 和第一次一样
```

然后把 `agent.json` 里的 `tls_cert`、`tls_key` 改成 `tls-new/server.pem`、`tls-new/server-key.pem`，重启 Agent。`--ca-dir` 只读旧目录；新目录里的 `ca.pem` 和 `ca-key.pem` 和旧的一字不差。CA 本身不到 30 天就过期时，工具会拒绝（那时只能重新生成 CA，重新配对）。

**换一台电脑怎么办？** 设备里存着 Agent 的地址、CA 证书和令牌：CA 和令牌属于“那个 Agent”（它的 `tls/` 目录和数据库），不属于某台电脑的硬件。

- **设备不用动：** 停掉旧 Agent，把整个 `~/.config/deskhub/`（`agent.json`、`tls/`、`state/`）复制到新电脑的同一位置，让新电脑用**和旧电脑相同的 IP**（在路由器里把这个 IP 改给新电脑），再启动。
- **IP 换了：** 设备里存的是旧地址，设备要重新配对（USB，已配对的加 `--replace`）。用 `create_tls.py --ca-dir` 沿用旧 CA，浏览器里信任的 CA 就不用重新信任。
- **什么都不搬：** 在新电脑上重新 `init`、生成证书，所有设备重新配对。旧电脑上的令牌作废。
