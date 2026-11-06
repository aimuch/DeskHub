# 桌面 App（菜单栏）设计

状态：**设计中**（2026-11-06）。范围项是 [product-scope.md](product-scope.md) 的 N4。这份文档只写设计。没有 Swift 代码，Agent 也没有改。
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
