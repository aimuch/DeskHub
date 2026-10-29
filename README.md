# DeskHub

DeskHub 是放在电脑桌上的一块触摸屏，加上电脑里跑的一个小程序（Agent）。

它做三件事：

1. 用触摸屏切换显示器的输入源（只支持 macOS + BetterDisplay）。
2. 显示电脑的状态：CPU、内存、网络。
3. 当桌面时钟，并显示和管理设备自己的亮度、电池和连接。

硬件是 Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 圆角 AMOLED 触摸屏，带电池）。
电脑端只支持 macOS。

## 现在能用什么

| 功能 | 状态 |
| --- | --- |
| 设备界面：总览、动作、电脑、设置、电池与电源、连接、外观、显示、时钟 | 做完了。你在真机上看过“显示正常”；触摸、旋转、逐页检查还没有逐项验证。 |
| 用 USB 把设备连到电脑（Wi-Fi、Agent 地址、证书） | 工具在。重做之后的版本还没有在真机上完整走过一遍。 |
| 电脑端 Agent：接口、命令行、显示器动作 | 做完了，本机跑通过。显示器真的切出、切回，没有证据。 |
| 看电脑状态（CPU、内存、网络） | 代码在。要设备连上 Agent；没有真机验证。 |
| 充电保护（实验功能，默认关） | 做完了，主机测试全过；**没有在真机上验证**。 |
| 无线配网（长按按键开热点） | 电脑端做完了，**设备端没接线，不能用**。现在只能用 USB 配对。 |
| 网页管理台 | **没有**。旧版有，在 `archive/v0.2` 分支。 |
| 管理员密码登录 | **没有**。 |
| 无线固件更新（OTA） | 代码在，没有真机验证。不要依赖它。 |
| 声音 | 没做。 |

详细的范围和决定在 [产品范围](docs/product-scope.md)。

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
想让它开机自启，看 [agent/README.md](agent/README.md) 的“macOS 自启动”。

### 4. 刷固件

要有固件包 `deskhub-<版本>-esp32s3.zip`。它由 `scripts/package_firmware.py` 生成，见 [开发笔记](docs/development.md)。
解压后，在解压目录：

```sh
python3 -m venv .flash-venv
.flash-venv/bin/python -m pip install -r requirements-flash.txt
.flash-venv/bin/python flash.py --check
.flash-venv/bin/python flash.py --list
.flash-venv/bin/python flash.py --port /dev/cu.usbmodem……    # 换成 --list 显示的端口
```

`--check` 不碰设备。`flash.py` 不会擦掉设备上已有的配置。
端口找不到、写入失败，看 [docs/flashing.md](docs/flashing.md)。

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

## 日常使用

### 在设备上

- 底部四个按钮：**总览、动作、电脑、设置**。
- **顶栏全是图标。** 左边是 Wi-Fi 和电脑：图标上有一道斜杠，表示没配置或没连上；Wi-Fi 图标闪烁，表示正在连接。
  右边是 USB、电池：有 USB 供电才显示 USB 图标；电池里的数字是电量（不带 %），充电时电池左边有一个闪电。电量未知时数字是问号，不会显示成 0。
  点左边进“连接与配对”，点右边进“电池与电源”。
- **总览**：时钟、四段状态环（Wi-Fi、电脑、电源、时间）、两张常用动作卡片。点时钟进“桌面时钟”。点动作卡片执行动作。
- **设置**里有：显示与亮度（亮度、常亮、息屏、夜间限亮、旋转）、外观（主题、强调色、中文或 English、文字大小、**电池百分比**显示或隐藏、**圆环粗细**细、标准、粗）、连接与配对、电池与电源、桌面时钟、设备信息。
- **设备信息**里可以重启和恢复出厂。恢复出厂要点两次，第一次不会删任何东西。
- 屏幕关了以后，第一次触摸只唤醒屏幕，不会触发按钮。
- 屏幕四角是圆角，界面会躲开它。如果你还看到有字被切，请告诉我哪个角、哪个字，我调一个参数（`kCornerR`）。

### 电池与电源

- **电量** 是电源芯片的估算，没有对照过这颗电芯。读不到时显示“电量未知”，不会显示成 0%。
- **读数** 显示电池电压、USB 电压和芯片温度。芯片温度是电源芯片的温度，不是电池的。
- **保护** 是充电保护（**还没有在真机上验证过**）：电量到上限（80% 或 90%）就停止充电，低于回充线再恢复。默认关闭，开启要点“开启…”再确认。
  读不到电量、数据过期或写入失败时，它会恢复正常充电，并在屏幕上说明。
  **刷回 0.3.1 或更早的固件之前，先在这里关闭它**，原因见 [docs/flashing.md](docs/flashing.md)。

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
4. 照 [agent/examples/display-binding.json](agent/examples/display-binding.json) 把 `displays` 填好。`inputs` 要填你这台显示器真实的输入源数值，**没有默认值**。
5. 在 `actions` 里加动作（例子在 [agent/README.md](agent/README.md)），重启 Agent，再给设备配对 `control:displays`。

细节和安全规则在 [agent/README.md](agent/README.md)。

## 常见问题

**找不到设备的串口。** 先换一根能传数据的 USB 线，关掉别的串口工具。还不行：按住 BOOT，插 USB，松开 BOOT，再 `flash.py --list`。

**Agent 命令报 `ConnectError`。** 最常见的原因是地址和证书对不上：`--url` 或 `agent_url` 里的地址，要在第 2 步的 `--dns` 或 `--ip` 里。其次：Agent 没在运行；`allowed_networks` 没包含设备所在的网段；Mac 的防火墙挡住了。

**设备显示“电脑离线”。** 同上。另外确认设备和 Mac 在同一个 2.4 GHz 网络，Wi-Fi 密码没输错。设备会自动重连。

**想重新配对。** 再运行第 5 步的命令，加 `--replace`。`deskhubctl devices` 列出已配对的设备，`deskhubctl revoke 设备编号` 吊销一个。

**刷机以后设备一直不充电。** 你可能带着“停充”状态刷了旧固件。刷回新固件，它启动时会恢复充电。

## 还没有的功能

网页管理台、管理员密码登录、设备上的无线配网、声音、Windows 和 Linux 的显示器支持。
[产品范围](docs/product-scope.md)里写了每一项的状态和为什么。

## 更多文档

| 文档 | 内容 |
| --- | --- |
| [产品范围](docs/product-scope.md) | 做什么、不做什么、每个功能的状态、决策记录 |
| [规则](docs/rules.md) | 安全规则，以及放宽的规则和代替它的保护 |
| [刷机说明](docs/flashing.md) | 在另一台电脑上用 USB 刷机；刷旧固件前的注意事项 |
| [Agent 说明](agent/README.md) | Agent 的命令、权限、显示器后端、自启动（里面的“网页管理台”一节写的是旧版，main 上没有） |
| [充电保护设计](docs/charge-protection-design.md) | 充电保护的规则、失败模式和验证状态 |
| [配对接口设计](docs/pairing-design.md) | 无线配网（热点）的设计和进度 |
| [设计稿](docs/design/README.md) | 设计稿源文件和设计变量 |
| [开发笔记](docs/development.md) | 给改代码的人：分支、安全基线、构建、测试、改动记录 |

`.state/` 里存着签名私钥、凭据和 Agent 数据库，被 `.gitignore` 排除。**不要提交，也不要复制出去。**
