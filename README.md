# DeskHub

DeskHub 是放在电脑桌上的一块触摸屏，加上电脑里跑的一个小程序（Agent）。

它做两件事：

1. 用触摸屏切换显示器的输入源（只支持 macOS + BetterDisplay）。
2. 当桌面时钟，并显示和管理设备自己的亮度、电池和连接。

设备不再显示电脑的 CPU、内存和网络（0.3.17 起）。

硬件是 Waveshare ESP32-S3-Touch-AMOLED-2.16（480×480 圆角 AMOLED 触摸屏，带电池）。
电脑端只支持 macOS。

## 现在能用什么

| 功能 | 状态 |
| --- | --- |
| 设备界面：总览、动作、电脑、设置、电池与电源、连接、外观、显示、时钟 | 做完了。你在真机上看过“显示正常”；触摸、旋转、逐页检查还没有逐项验证。 |
| 新界面 v2：页面和首页可调、切换页列表或图标、输入源改名字和图标、夜航主题 | 0.3.17 已刷到设备（2026-11-07），读回 ui_alive true、Wi-Fi 已连接。屏幕上还没有人看过。 |
| 桌面 App（菜单栏，DDC 切源） | 第一版 0.1.0 做好了（2026-11-07），在 [desktop/](desktop/)。你在真机上绑定、试切、从菜单切换过，读回都确认。触摸屏还不能连它（蓝牙要等固件）。 |
| 用 USB 把设备连到电脑（Wi-Fi、Agent 地址、证书） | 工具在。重做之后的版本还没有在真机上完整走过一遍。 |
| 电脑端 Agent：接口、命令行、显示器动作 | 做完了，本机跑通过。显示器真的切出、切回，没有证据。 |
| 充电保护（实验功能，默认关） | 做完了，主机测试全过。0.3.6 在真机上到 90% 停过一次充（设备自己报告，没有用仪表对照）；**降到回充线以下再恢复充电，还没有验证**。 |
| 无线配网（手机连设备热点） | 电脑端和设备端的代码都做完了，主机测试过；**设备端没有在真机上验证**，按键引脚也没核对原理图。现在请用 USB 配对。 |
| 网页管理台 | 做完了：九个页面加登录。用真实的 Agent 和 Chrome 走过完整流程（62 项）；**没有接真实设备和显示器**。 |
| 管理员密码登录 | 做完了：第一次用口令，马上设置密码；输错 5 次锁 15 分钟；忘记密码在电脑上用命令行重置。 |
| 无线固件更新（OTA） | 代码在，没有真机验证。不要依赖它。 |
| 声音（提示音，默认关；音量三档） | 做完了：0.3.7 已刷到设备，2026-10-31 你听到了试听音。音量三档是 0.3.8 加的，已在设备上（2026-11-02 核对过），三档还没有人听过。提示音在动作和配对时响不响，还没有在真机上试。 |
| 屏幕软键盘更换 Wi-Fi | 已实现，0.3.13 已刷到设备（2026-11-03）。设置 → 连接与配对 → 更换 Wi-Fi；先试连再保存，失败时尝试回到原网络。既有显示条带已定位到 DMA 临时内存不足和启动命令冲突；已修复，驱动日志未再报错，用户已确认显示正常。USB 已读回 Wi-Fi 已连接；失败回退和 Agent 连通仍未完整验证。首次配对仍用 USB 或手机热点。 |
| 屏幕上开始配对；已存 Wi-Fi 时开机不再开热点 | 0.3.15 已刷到设备：开机没有自动开热点，Wi-Fi 一直连着。屏幕上的“开始配对”还没有试。设置 → 连接与配对 → 开始配对。 |
| 时区、手动校时 | 0.3.15 已刷到设备，还没有在屏幕上试。设置 → 日期与时间。 |

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

- 底部按钮是你打开的页面，最后是**设置**。默认是 **切换、总览、设置**。可以打开的页面有四个：
  **切换**（输入源）、**总览**（状态环）、**时钟**、**电源**。在“设置 → 页面与首页”里开关和排序。
  **第一个打开的页面是首页**：开机显示它；屏幕空闲进了时钟页，醒来回到它。最后一个打开的页面不能关。（0.3.17）
- **顶栏全是图标。** 左边是 Wi-Fi 和电脑：图标上有一道斜杠，表示没配置或没连上；Wi-Fi 图标闪烁，表示正在连接。
  右边是 USB、电池：有 USB 供电才显示 USB 图标；电池里的数字是电量（不带 %），充电时数字前面有一个闪电（画在电池里）。电量未知时数字是问号，不会显示成 0。
  点左边进“连接与配对”，点右边进“电池与电源”。
- **连接与配对**：写当前 Wi-Fi 的名称和信号。点“更换”换 Wi-Fi；点“开始配对”打开配对热点（已配对时写“重新配对”），这时 Wi-Fi 会暂时断开。设备编号在“设备信息”页。
- 没有存 Wi-Fi 的设备，开机 8 秒后自动打开配对热点。已经存了 Wi-Fi 的设备不会自动打开。
- **切换**：显示器的输入源。点一个就切过去。样式在“设置 → 输入源”里选：列表或图标。
  点“设置 → 输入源”里的一个输入源，可以改它在设备上的**名字**（软键盘，只有英文字母、数字和符号；中文名要在 App 里改）
  和**图标**（HDMI、DP、USB-C、笔记本、主机、显示器、游戏机、电视）。“恢复 App 的名字和图标”撤销改动。（0.3.17）
- **总览**：时钟和四段状态环，在屏幕中间。点时钟进“桌面时钟”。
  **状态环**四段是：右上青色 = 网络（Wi-Fi 已连接），右下绿色 = 电脑（已连上 Agent），左下橙色 = 电源（读数是新的，没有故障），左上紫色 = 时间（已校准）。亮表示正常，暗表示没有数据或有问题。
  **点圆环**（时钟除外）打开说明页：每一段的名字、状态和意思，暗的那一段写原因。圆环下面那一行：某一段暗了，写它的名字（例如“电脑没连上”）；都正常时写“点圆环看说明”。
- **时钟**：大号时间、日期、60 格刻度和分钟点；下面四个小圆点和状态环一样，亮表示正常。
- **设置**里有：页面与首页、输入源、显示与亮度（亮度、常亮、息屏、夜间限亮、旋转）、外观（主题：夜航、薄荷夜色、深海蓝、暖橙；强调色、中文或 English、文字大小、**电池百分比**显示或隐藏、**圆环粗细**细、标准、粗）、连接与配对、电池与电源、声音（提示音、音量、试听）、桌面时钟、日期与时间（时区、手动校时）、设备信息。
- **设备信息**里可以重启和恢复出厂。恢复出厂要点两次，第一次不会删任何东西。恢复出厂会删掉 Wi-Fi、配对、显示配置、时区、声音和外观设置（0.3.15 起；以前声音和外观会留下来），以及输入源的名字和图标（0.3.17 起）。
- 从 0.3.16 或更早升级到 0.3.17：外观变成夜航主题，原来主题的颜色留作强调色。想用原来的主题，在“设置 → 外观”里选回去。
- 屏幕关了以后，第一次触摸只唤醒屏幕，不会触发按钮。
- 屏幕四角是圆角，界面会躲开它。如果你还看到有字被切，请告诉我哪个角、哪个字，我调一个参数（`kCornerR`）。

### 电池与电源

- **电量** 是电源芯片的估算，没有对照过这颗电芯。读不到时显示“电量未知”，不会显示成 0%。
- **读数** 显示电池电压、USB 电压和芯片温度。芯片温度是电源芯片的温度，不是电池的。
- **保护** 是充电保护（0.3.6 在真机上停过一次充，**恢复充电还没有验证**）：电量到上限（80% 或 90%）就停止充电，低于回充线再恢复。默认关闭，开启要点“开启…”再确认。
  读不到电量、数据过期或写入失败时，它会恢复正常充电，并在屏幕上说明。
  **刷回 0.3.1 或更早的固件之前，先在这里关闭它**，原因见 [docs/flashing.md](docs/flashing.md)。

### 声音

- **设置 → 声音**：“提示音”开关，默认关。打开后，动作有结果时响一声：确认了是一高一低的短音，没确认或失败是两声往下的短音。配对完成也响确认音。
- 夜间限亮打开时，夜间时段里不响（时间未知时也不响）。两次声音至少隔 0.6 秒。
- **音量**有三档：低、中、高，默认低（就是 0.3.7 的固定音量），每档差 10 dB。点一档马上存下，并用这一档响一声；存不下会写“没有保存，请再试一次”，音量不变。提示音和配对完成的音都用这一档。
- **试听** 不管开关和夜间，都响一次确认音。
- 设备启动时声音芯片没有应答，页面会写“没有检测到扬声器”，开关和试听变暗。
- 在电脑上让设备响一次（程序听不到，响没响要你听）：

```sh
.venv/bin/python tools/provision.py --port /dev/cu.usbmodem…… --sound-test
```

2026-10-31 你听到了 0.3.7 的试听音。三档还是太轻或太响，告诉我，三档的数值可以改。

### 日期与时间

- **设置 → 日期与时间**。时区：点 − 和 + 切换，马上保存，时钟马上变。默认 UTC+8。设备不处理夏令时，要你自己改。
- 手动校时：点年、月、日、时、分里的一段，用 -1 和 +1 改，再点“保存时间”（要往下滑一点）。联网后，设备会用网络时间。

### 网页管理台

`deskhubctl serve` 启动时会打印管理台的地址，形如 `https://你的Mac局域网IP:8765/manage/`。

1. **第一次**：用 `deskhubctl admin-token --output admin.json` 生成管理员口令，打开管理台，选“口令（首次）”，粘贴 `admin.json` 里的 `token`。
2. 登录后**必须马上设置密码**（至少 12 个字符，不能是常见的弱密码，也不能和口令相同）。设置完以前，其他页面都打不开。
3. 以后只用密码登录；网页不再接受口令，口令留给命令行和脚本。输错 5 次，网页登录锁 15 分钟。
4. **忘记密码**：在运行 Agent 的电脑上执行 `deskhubctl --config ~/.config/deskhub/agent.json admin-password-reset`。网页上没有“忘记密码”。
5. 页面：概览、设备、动作与显示器、命令记录、布局与显示、电源、固件升级、访问管理、外观。没有数据的地方写“未上报”，过期的数据写“数据已过期”。
6. 浏览器提示证书不受信任：见登录页右边的“信任证书”三步。

管理台的设计和限制见 [docs/web-console-design.md](docs/web-console-design.md)。

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

## 还没有的功能

声音和设备上的无线配网的真机验证，Windows 和 Linux 的显示器支持。网页上还不能开关充电保护：设备的上报里没有这一项。
[产品范围](docs/product-scope.md)里写了每一项的状态和为什么。

## 更多文档

| 文档 | 内容 |
| --- | --- |
| [产品范围](docs/product-scope.md) | 做什么、不做什么、每个功能的状态、决策记录 |
| [规则](docs/rules.md) | 安全规则，以及放宽的规则和代替它的保护 |
| [刷机说明](docs/flashing.md) | 在另一台电脑上用 USB 刷机；刷旧固件前的注意事项 |
| [Agent 说明](agent/README.md) | Agent 的命令、权限、显示器后端、自启动（里面的“网页管理台”一节写的是旧版，main 上没有） |
| [充电保护设计](docs/charge-protection-design.md) | 充电保护的规则、失败模式和验证状态 |
| [桌面 App 设计](docs/desktop-app-design.md) | macOS 菜单栏 App 和 DDC 切源的设计（设计中，有待你决定的问题） |
| [配对接口设计](docs/pairing-design.md) | 无线配网（热点）的设计和进度 |
| [设计稿](docs/design/README.md) | 设计稿源文件和设计变量 |
| [开发笔记](docs/development.md) | 给改代码的人：分支、安全基线、构建、测试、改动记录 |

`.state/` 里存着签名私钥、凭据和 Agent 数据库，被 `.gitignore` 排除。**不要提交，也不要复制出去。**
