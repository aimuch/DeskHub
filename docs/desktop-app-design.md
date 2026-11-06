# 桌面 App（菜单栏）设计

状态：**设计中**（2026-11-06）。**第 12 节改了方向：触摸屏通过 BLE 直连 App，Agent 变成可选。第 3 到 5 节里“App 经过 Agent”的部分，以第 12 节为准。**范围项是 [product-scope.md](product-scope.md) 的 N4。这份文档只写设计。没有 Swift 代码，Agent 也没有改。
标“我定的”的地方，是规则没有规定、由我选的值，请你审。第 11 节的问题，在你决定之前不实现。

## 1. 为什么做

- 现在要连电脑，得在终端里做 5 件事：`deskhubctl init`、`tools/create_tls.py`、手改 `agent.json`、`deskhubctl serve`、`tools/provision.py`。你觉得太麻烦。
- 电脑上已经能切源（`deskhubctl run <action_id>`、快捷键），但要先单独配一份命令行凭据，而且没有图形界面。
- 切源现在要装 BetterDisplay。BetterDisplay 是闭源商业软件，不能参考它的代码，只能调用它公开的命令行。

## 2. 做什么，不做什么

做（第一阶段）：一个 macOS 菜单栏 App。它是 Agent 的图形外壳：
- 看 Agent 是否在运行、设备是否在线。
- 一键执行动作，包括切源。
- 启动、停止 Agent；开关开机自启。
- 全局快捷键执行动作。
- 打开网页管理台。

不做：
- **不重写 Agent。** Agent 仍是现在的 Python 程序。设备连的、做决定的，都还是它。App 只调用它的接口。
- **不把功能做进 Status Trio。** 那是别人的仓库，你没有写权限；方向不同；它很大，更新频繁，维护分支成本高。
- **不用 BetterDisplay 的代码。** 它没有公开源码，也没有许可证。
- 不上 App Store（DDC 要用私有 API，见第 5 节）。
- 不支持 Intel Mac 的 DDC 切源（AppleSiliconDDC 和 m1ddc 都只支持 Apple Silicon）。
- 不把 Agent 端口暴露到公网（规则第 3 节）。

## 3. 结构

```
菜单栏 App（Swift）──HTTPS + 令牌──> Agent（Python）──> 切源后端（BetterDisplay 或 DDC 助手）──> 显示器
                                         ^
设备（ESP32）──────HTTPS + 令牌──────────┘
```

- App 和设备一样，是 Agent 的一个客户端。App 有自己的令牌，可以单独吊销（K1）。
- App 用的地址和 CA，和 `deskhubctl` 命令行凭据相同：`agent_url` 加 `ca_file`。App 只信这个 CA，不关闭证书检查（K1）。
- 切源只走 Agent 的 `POST /api/v1/commands`。所以触摸屏、命令行、快捷键、App 用同一套动作白名单和同一条切源路径（K2）。
- App 自己不直接写显示器。理由：如果 App 也能直接切源，就有两条路径、两套绑定和两种结果，K2、K3 都要再审一遍。

## 4. 第一阶段的功能

| 功能 | 怎么做 | 用到的接口 |
| --- | --- | --- |
| 状态 | 菜单栏图标三种样子：Agent 在线、设备在线；Agent 在线、设备不在线；Agent 不在线。打开菜单写原因 | `GET /api/v1/status`、`GET /api/v1/capabilities` |
| 动作 | 菜单里列出动作，点一个就执行。结果按 K3 写：读回一致才写“已确认”，否则写“已发送（未确认）” | `GET /api/v1/actions`、`POST /api/v1/commands`、`GET /api/v1/commands/{id}` |
| 启动和停止 | 用 launchd 的 LaunchAgent 管理 `deskhubctl serve`，模板沿用 `agent/packaging/com.deskhub.agent.plist.in` | `launchctl` |
| 开机自启 | App 自己：`SMAppService.mainApp`。Agent：上面的 LaunchAgent | 系统接口 |
| 快捷键 | 给每个动作配一个全局快捷键 | Carbon `RegisterEventHotKey` |
| 管理台 | 打开 `https://<agent>/manage/` | 浏览器 |
| 只开一个 | 第二次打开时，把已有的那个带到前面 | 文件锁 |

菜单的样子（我定的，要先画设计稿再实现）：

```
● Agent 在线 · 设备在线
─────────────
切到 Mac          ⌃⌥1
切到 PC           ⌃⌥2
─────────────
最近：切到 PC · 已发送（未确认）
─────────────
打开管理台
Agent：停止 / 开机自启 ✓
App 开机自启 ✓
退出
```

一次只能有一个命令在执行。上一个还没有结果时，动作变暗（K2：不自动重试，不叠加）。

## 5. 切源后端

### 5.1 三个选择

| 后端 | 怎么切 | 能读回吗 | 要装什么 | 风险 |
| --- | --- | --- | --- | --- |
| BetterDisplay（现有） | `set --feature=ddc --vcp=0x60`（`agent/src/deskhub_agent/backend.py`） | 能（`get ... --vcp=0x60 --value`） | BetterDisplay，部分功能要 Pro | 闭源；真实显示器上没有证据 |
| m1ddc（MIT，命令行） | `m1ddc display <uuid> set input <n>`；LG 用 `input-alt` | **不能**：没有 `get input` | m1ddc | 读不回，所以永远只能写“已发送（未确认）” |
| AppleSiliconDDC（MIT，Swift 库加 `asddc` 命令行） | `setvcp` 写 VCP 0x60 | 能：`getvcp 0x60` | 我们自己编译的一个小助手 | 私有 API |

m1ddc 和 AppleSiliconDDC 都是 BetterDisplay 作者开源的，许可证是 MIT。

### 5.2 建议（我定的）

DDC 放进 **Agent**，不放进 App：
- 新增后端 `ddc`，和 `betterdisplay` 并列，`agent.json` 里二选一。
- 后端调用一个小助手 `deskhub-ddc`。它用 AppleSiliconDDC 写成，只做三件事：列出显示器、写 VCP 0x60、读 VCP 0x60。参数由 Agent 从本地配置拼出，不接受客户端传来的参数（K2）。
- 这样触摸屏也能用 DDC 切源，不用装 BetterDisplay。
- 选 AppleSiliconDDC，不选 m1ddc，是因为它能读回。没有读回，K3 就永远写不出“已确认”。

助手怎么编译、放在哪里，是第二阶段打包的一部分。在那之前，可以先用 m1ddc 做一次实测，看你的显示器认哪些数值。

### 5.3 DDC 的限制

- 只支持 Apple Silicon。
- 用的是私有 API（IOAVService）。macOS 升级后可能失效；不能上 App Store。
- 输入源数值因显示器而异。标准值：DP1=15、DP2=16、HDMI1=17、HDMI2=18、USB-C=27。LG 等显示器用另一套（`input-alt`：DP1=208、HDMI1=144……）。
- 很多显示器读回当前输入不准。AppleSiliconDDC 自己也提醒，有些显示器只部分遵守 MCCS。
- **切到别的输入以后，这台 Mac 可能就收不到这台显示器的 DDC 了，也就切不回来。** 回来只能靠显示器按键，或者另一台电脑。现有的 `return_path_verified` 字段就是记这件事的，必须实测后才设为真。

### 5.4 绑定

现在显示器按 UUID、序列号、注册路径绑定（K2，`DisplayBinding`）。DDC 后端也要这三样：
- UUID：AppleSiliconDDC 和 m1ddc 都能按系统 UUID 选显示器。
- 序列号：m1ddc 的 `display list detailed` 有；AppleSiliconDDC 要实测。
- 注册路径：**还不知道** DDC 库能不能给出和 BetterDisplay 一样的值。给不出，就要改绑定规则，按 K7 单独审。这是第 11 节的问题 4。

## 6. 参考 Status Trio 的哪些部分

Status Trio（lingyired，Apache-2.0，Swift，macOS 13 以上）。只参考做法，不整份复制：

| 要做的事 | Status Trio 里的做法 |
| --- | --- |
| App 开机自启 | `LaunchAtLoginManager.swift`：包一层 `SMAppService.mainApp`，测试里用假的实现 |
| 只开一个 | `SingleInstanceGuard.swift`：文件锁，并区分“已经有一个在运行”和“锁不能用” |
| 全局快捷键 | `GlobalShortcutController.swift` 加 `CarbonHotKeyBackend.swift` |
| 自动更新（第二阶段） | `UpdaterManager.swift`（Sparkle） |
| 调用 BetterDisplay | `BetterDisplayIntegrationClient.swift`：用 `pro.betterdisplay.BetterDisplay.request` 和 `response` 这两个分布式通知，不起子进程 |
| DDC | `Sources/DDCPrivateAPI/`，来自 AppleSiliconDDC（MIT，作者 Istvan T.） |

许可证：
- 复用 Status Trio 的代码：保留 Apache-2.0 许可证全文和它的 NOTICE（“Status Trio, Copyright 2026 lingyired”），在改过的文件里写明改动。
- 复用 AppleSiliconDDC 或 m1ddc：保留 MIT 版权声明。AppleSiliconDDC 的作者希望用了就公开提一下，在 README 写明。
- 不用 Status Trio、BetterDisplay 的名字、图标和品牌。

## 7. 失败模式

先写会怎么出错，再写代码。每一条都要有测试，或者在真机上看。

| 编号 | 会怎么出错 | 设计怎么挡住 |
| --- | --- | --- |
| D1 | Agent 没运行，App 还显示上次的动作和结果 | 连不上就写“Agent 不在线”，动作变暗，不显示旧结果（K3） |
| D2 | 证书对不上（IP 换了、CA 换了），App 改成不检查证书 | 不提供“忽略证书”。写“证书和地址对不上”，指向设置步骤（K1） |
| D3 | App 的令牌被吊销，App 还在重试 | 收到 401（`UNAUTHORIZED`）就停，写“凭据已被吊销，请重新配对”，不自动重试（K2） |
| D4 | 令牌进了日志、崩溃报告或命令行参数 | 令牌只存在钥匙串（Keychain）里；导入后清零内存；任何日志都只写原因码（K4） |
| D5 | 连点两下，发出两个切源命令 | 上一个命令没有结果时，动作变暗；不自动重试（K2） |
| D6 | 切源后读回不一致，或读不回，却写“已确认” | 只照搬 Agent 的结果：状态是 `succeeded` 才写“已确认”，`unconfirmed` 写“已发送（未确认）”（K3） |
| D7 | BetterDisplay 没装或没开 | 照搬 Agent 的 `BACKEND_NOT_INSTALLED` 等原因码，写成人话 |
| D8 | DDC 不支持这台显示器，或输入源数值不对 | 只用本地配置里实测过的数值；`compatibility_verified` 为假时，动作写“未验证” |
| D9 | 切走以后切不回来 | `return_path_verified` 为假时，菜单里写明“切走后可能要用显示器按键切回” |
| D10 | 开了两个 App，各发一个命令 | 只开一个（文件锁）；Agent 本身也一次只跑一个命令 |
| D11 | 快捷键和别的软件冲突，注册失败但不说 | 注册失败就写明哪个快捷键没注册上 |
| D12 | App 停了 Agent，设备掉线，但没提醒 | 停止前确认：“设备会断开”；停止后状态马上变 |
| D13 | LaunchAgent 的 plist 写错路径，开机启动失败，没人知道 | 写入后读回并检查路径；启动后 10 秒内读 `/healthz`，失败就写原因 |
| D14 | macOS 升级后 DDC 失效，App 只说“失败” | Agent 读不到显示器时报 `TARGET_UNAVAILABLE`，App 写“DDC 现在不可用，可能是系统升级”，并建议改用 BetterDisplay 后端 |
| D15 | 有人在局域网里冒充 Agent | 只信本地的 CA；地址必须和凭据里的 `agent_url` 一致（K1） |

## 8. 和规则的对应

| 规则 | 这个 App 怎么做 |
| --- | --- |
| K1 信任链 | App 只信私有 CA，有自己的令牌，可以单独吊销 |
| K2 命令安全 | 只执行 Agent 的白名单动作；不自动重试；DDC 助手不接受客户端参数 |
| K3 不伪造数据 | 只显示 Agent 给的结果；未知写未知，过期写过期 |
| K4 秘密 | 令牌只放钥匙串，不进日志、参数和网址 |
| K7 基线文件 | 给 Agent 加 DDC 后端会改 `backend.py`、`models.py`（`backend` 的取值），要单独提交并补测试 |
| R6 管理台 | 打开管理台的地址和现在一样，不新增绕过登录的路 |
| 永远不做 | 不暴露公网端口；不执行任意命令 |

## 9. 第二阶段（可选）：设置向导和打包

目标：不打开终端，点几下就连上设备。

向导要代替的步骤：
1. 生成配置（`deskhubctl init`）。
2. 生成证书（`create_tls.py`），自动填本机的局域网 IP；提醒你在路由器里固定这个 IP。
3. 改 `agent.json`（`bind`、`lan_enabled`、证书、`allowed_networks`）。
4. 启动 Agent，设成开机自启。
5. USB 配对：Wi-Fi 密码由你在 App 里输入，App 用安全输入框，不进日志（K4）。
6. 给 App 自己发一份令牌，存进钥匙串。

打包的成本（我估的）：

| 做法 | 成本 | 问题 |
| --- | --- | --- |
| Agent 仍用 uv 装在仓库里，App 只调用它 | 小 | 还是要先装 Python 和 uv |
| 把 Python 运行时和 Agent 打进 App（py2app 或 PyInstaller） | 中到大 | App 变大（大约几十 MB）；每个 Python 依赖都要签名；FastAPI、uvicorn 的打包要实测 |
| 用 Swift 重写 Agent | 很大 | 要重做全部测试和安全审查，不建议 |

签名和公证：
- 只给自己用：不公证也能运行，第一次打开时要在系统设置里允许。
- 给别人用：要 Apple 开发者账号（每年 99 美元），App 和里面的每个可执行文件都要签名，再提交公证。

## 10. 验证计划（真机）

1. 先不写代码：用 m1ddc 在你的显示器上试 `set input`。记下你的每个输入对应哪个数值，以及切走后能不能切回来。
2. 用 AppleSiliconDDC 的 `asddc getvcp 0x60` 看读回准不准。
3. 再决定后端和绑定规则，然后才写 Agent 的 DDC 后端和 App。
4. 每个功能都在真机上走一遍，结果写进 `docs/evidence/`。

## 11. 要你决定的事

1. **放在哪里。** 我建议本仓库的 `desktop/` 目录（Swift Package），和 `agent/`、`firmware/` 并列。
2. **切源用哪个后端。** BetterDisplay、DDC，还是两个都支持、在 `agent.json` 里选。我建议两个都支持，默认 DDC。
3. **DDC 放在哪里。** 我建议放进 Agent（第 5.2 节），这样触摸屏也能用。另一个选择是只放进 App，那触摸屏就继续靠 BetterDisplay。
4. **绑定规则。** DDC 库如果给不出注册路径，是否允许只用 UUID 加序列号绑定。这要改 K2 的写法，要你同意。
5. **要不要公证。** 只给自己用就不需要。
6. **要不要做第二阶段**（设置向导和打包）。
7. **菜单的设计。** 第 4 节的菜单只是草图。你同意方向后，我先画设计稿，再写测试和代码。

## 12. 方向调整（2026-11-06）

和你讨论后，方向改成下面这样。第 3 到 5 节里“App 经过 Agent”的部分，以这一节为准。

### 12.1 改了什么

| 项 | 原来 | 现在 |
| --- | --- | --- |
| 电脑状态（CPU、内存、网络） | Agent 读，设备显示 | **不做了**（你说没用） |
| 触摸屏连谁 | Wi-Fi 连 Agent | **BLE 直连 Mac 上的 App** |
| 切源由谁做 | Agent 调 BetterDisplay | **App 用 DDC 直接切**（DDC 代码在 App 里） |
| 菜单栏切源 | App → Agent → 后端 | App 自己切；和触摸屏走同一段代码 |
| Wi-Fi | 连 Agent | **长期保留**，给网络校时、固件更新、以后的新闻和天气用 |
| 设 Wi-Fi | USB 或热点配网 | 屏幕软键盘（已有），或 App 通过 BLE 发给设备 |
| Agent | 必须 | **可选**：只有要网页管理台、多台电脑时才装 |

为什么还是只有一条切源路径：触摸屏的命令经 BLE 到 App，菜单栏的命令也在 App 里，两者调用 App 里同一个切源函数。绑定、白名单、读回判断只在这一处（K2、K3）。

### 12.2 新的结构

```
触摸屏 ──BLE（加密配对）──> App ──DDC──> 显示器
   │                          ↑
   │                     菜单栏、快捷键
   └──Wi-Fi──> 公网（网络校时、以后的天气和新闻、固件更新）
```

### 12.3 BLE 的安全（要像 R7 那样先设计，再审）

- 配对：LE Secure Connections，数字比较。设备屏幕显示 6 位数字，Mac 上弹出同样的数字，你核对一致后两边确认。
- 只和配对过的 Mac 通信（bonding）；可以在设备上删除配对。
- 设备只接受白名单里的动作编号，不接受任意命令（K2）。
- 通过 BLE 发 Wi-Fi 密码只在加密链路上做，不进日志（K4）。
- K1 现在只写了“Wi-Fi + TLS + 令牌”，要加一条 BLE 的信任链。这条规则改动要你同意。

### 12.4 实验

| 编号 | 问题 | 结果 |
| --- | --- | --- |
| E1 | 画面切到别的电脑后，这台 Mac 还能用 DDC 切回来吗？ | **通过**（2026-11-06）。U2720QM：Mac mini 接 DP，HDMI 接 MacBook M1，USB-C 接 MacBook M4；切到 HDMI 和 USB-C 后，都由 Mac mini 切回 DP 成功，窗口没有被挪动。见 [ddc-return-path.json](evidence/ddc-return-path.json) |
| E2 | 屏幕、Wi-Fi、BLE 同时运行，内部内存够不够？屏幕会不会再出横纹？Wi-Fi 稳不稳？ | **没有通过**（2026-11-06），见 12.5 和 [ble-memory.json](evidence/ble-memory.json) |

E1 还说明了两件事：
- 这台显示器的数值是标准的：DP=15、HDMI=17、USB-C=27。
- m1ddc 的 `display list detailed` 给出系统 UUID、序列号和 IO 位置，正好对应现在的三项绑定（UUID、序列号、注册路径）。第 11 节的问题 4 可能不用改规则了。

E1 没有测的：读回当前输入（m1ddc 没有这个功能）、副屏 U2719DS、Mac 睡眠时、长时间稳定。

### 12.5 E2 的结果

测了 4 个只用于测试的版本（探针组件 `firmware/components/probe`，正式版里关闭）。每个版本采集 150 到 180 秒，中途换 4 次主题让整屏重画。

| 版本 | BLE | 内部内存空闲 | 最大连续块 | 开机以来最低 | 屏幕错误 |
| --- | --- | --- | --- | --- | --- |
| A：没有 BLE（对照） | 关 | 74 KB | 31.7 KB | 50.9 KB | 0 |
| B：默认配置 | **启动失败，头 37 秒反复重启** | 30–48 KB | 12–19 KB | 3.4 KB | **73 条，和以前的横纹是同一组错误** |
| C：省内存配置 | 蓝牙控制器分不到内存，启动失败 | 39 KB | 8.7 KB | 7.4 KB | 0 |
| D：BLE 最先启动 | 能广播，占 34 KB | 10–19 KB | **4.0–5.4 KB** | **1.85 KB** | 0 |

结论：
- **按现在的内存布局，BLE 放不进去。** D 能跑，但最大连续块只剩 4 KB 左右，正好卡在屏幕 DMA 每块约 4 KB 的边上，开机以来最低只剩 1.85 KB。再多分配一点，横纹就会回来。这不能当产品用。
- D 只测了广播。连上以后传数据还要更多内存，没有测：Mac 上的测试脚本一碰蓝牙就被 macOS 终止（命令行程序没有蓝牙权限声明）。
- 带蓝牙的版本 Wi-Fi 信号读数都更差（−62 到 −79 dBm，对照版 −35 到 −42）。原因没有确认。

### 12.7 E2 之后的建议（要你决定）

BLE 的好处是“不用折腾网络”。但真正麻烦的不是 Wi-Fi，而是 Agent 的证书、IP、配置文件和 USB 配对。Wi-Fi 本来就要留着（网络校时、以后的天气和新闻）。所以建议：

| 选择 | 做法 | 内存 | 麻烦程度 |
| --- | --- | --- | --- |
| **甲（推荐）：触摸屏通过 Wi-Fi 直连 App** | App 在局域网里提供一个很小的服务。设备用 mDNS 找到它。配对时设备屏幕显示 6 位数字，在 App 里核对确认，双方记住对方的公钥（第一次确认后固定）。不需要 Agent、不需要手动生成证书、不需要改配置文件 | 和对照版 A 一样，74 KB 空闲 | 低：配对一次，以后自动连 |
| 乙：先腾内存，再上 BLE | 先查清内部内存被谁占着（屏幕驱动、Wi-Fi、各任务的栈），把能挪的挪到 PSRAM，再测 BLE 已连接时的情况 | 不确定，可能腾不出 30 KB 以上 | 工作量大，结果不确定 |
| 丙：BLE 和 Wi-Fi 轮流开 | 平时只开 BLE；要校时、取天气时关 BLE、开 Wi-Fi | 关掉 Wi-Fi 才能腾出空间，要实测 | 复杂，切换时两边都会断 |

甲保留了你要的东西：DDC 在 App 里；触摸屏直连 App，不经过 Agent；画面切走后，触摸屏仍能切回来（E1 已通过）。它也去掉了你嫌麻烦的步骤。代价是触摸屏和 Mac 要在同一个局域网里。

### 12.6 第 11 节的问题，现在的状态

1. 放在哪里：仍建议 `desktop/`。
2. 切源后端：DDC 为主（E1 通过）。BetterDisplay 不再是必需。
3. DDC 放在哪里：**你选了放在 App 里，触摸屏通过 BLE 连 App。**
4. 绑定规则：m1ddc 能给出三项，可能不用改。还要确认 AppleSiliconDDC 能不能给出同样的值。
5. 要不要公证：只给自己用就不需要。
6. 第二阶段：设置向导的大部分步骤（证书、`agent.json`、USB 配对）不再需要，改成 BLE 配对。
7. 菜单和设备上的配对页：要先画设计稿。
8. **新增：** K1 加一条新的信任链，要你同意：选甲就是“局域网 + 屏幕数字配对 + 固定公钥”；选乙或丙是 BLE（12.3）。
10. **新增：** E2 没有通过以后，选甲、乙还是丙（12.7）。
9. **新增：** Agent、网页管理台、热点配网、证书工具变成可选以后，是保留、冻结，还是以后删除。
