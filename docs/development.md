# 开发笔记

给改代码的人看：分支、安全基线、基线文件的改动记录、构建和测试命令、现状的详细记录。
怎么安装和使用，看项目根目录的 [README](../README.md)。下面的命令都在仓库根目录运行。

## 分支

| 分支 | 内容 |
| --- | --- |
| `main` | 新设计。第一个提交只有 `.gitignore`、`LICENSE` 和本文件。第二个提交把下面的“安全基线”原样带入。 |
| `archive/v0.2`（标签 `legacy-v0.2.6`） | 旧版的完整归档：固件 0.2.6、Agent 0.2.0a1、旧管理台、旧设备界面、全部文档与证据、`dist/` 里每个发布包。只读保留，不合并进 `main`。 |

- 取回旧文件：`git checkout archive/v0.2 -- <路径>`
- 运行旧版：`git worktree add ../DeskHub-legacy archive/v0.2`
- 仓库目录外有单文件备份：`DeskHub-archive-v0.2.bundle`。

## 安全基线

下面的文件从 `archive/v0.2` **原样**带入，连同它们的测试。它们是已经验证过的安全与硬件事实，不从头重写。
修改其中任何文件，要单独提交，写明原因，并补测试。

| 部分 | 路径 |
| --- | --- |
| 签名与 OTA 校验 | `tools/firmware_package.py`、`agent/src/deskhub_agent/firmware_packages.py`、`firmware/components/management/` |
| USB 配对与秘密处理 | `tools/provision.py`、`tools/flash_bundle.py`、`tools/backup_flash.py`、`firmware/main/`、`firmware/components/core/include/json_util.hpp` |
| TLS 与 CA | `tools/create_tls.py`、`firmware/components/agent_client/` |
| 认证与访问控制 | `agent/src/deskhub_agent/` 下的 `app.py`、`management.py`、`models.py`、`storage.py`、`config.py` |
| 命令执行安全 | `agent/src/deskhub_agent/` 下的 `backend.py`、`scheduler.py`、`errors.py` |
| 固件安全底线 | `firmware/components/core/`、`firmware/partitions.csv`、`firmware/sdkconfig.defaults`、`firmware/dependencies.lock`、`firmware/CMakeLists.txt` |
| 真机得来的硬件事实 | `firmware/components/board_support/`、`scripts/ui_preview/touch_orientation_test.cpp` |
| 验收测试 | `firmware/tests/`、`agent/tests/`（不含旧管理台的 `test_web_assets.py`）、`tools/tests/`、`protocol/` |

Agent 的其余模块（`cli.py`、`metrics.py`、`snapshots.py`、`management_models.py`）和 `scripts/package_firmware.py`
也一并带入，因为上面的部分依赖它们。

检查“原样”：

```sh
git diff --stat archive/v0.2 main -- tools protocol firmware/components/core firmware/components/management firmware/components/agent_client firmware/main firmware/tests ':!firmware/tests/board_cache_test.cpp' ':!firmware/tests/CMakeLists.txt' ':!firmware/tests/ui_model_test.cpp' ':!firmware/tests/ui_preview' ':!firmware/tests/power_wire_test.cpp' ':!firmware/tests/pairing_test.cpp' ':!firmware/tests/charge_test.cpp' ':!firmware/main/CMakeLists.txt' ':!firmware/components/management/management.cpp' ':!firmware/components/management/CMakeLists.txt' ':!firmware/components/management/power_wire.cpp' ':!firmware/components/management/include/power_wire.hpp' ':!firmware/main/app_main.cpp' ':!tools/provision.py' ':!tools/tests/test_provision_power.py' ':!tools/tests/test_provision_charge.py' ':!protocol/openapi.json' ':!protocol/schemas/device-report.schema.json' ':!protocol/export_openapi.py' ':!protocol/README.md' ':!protocol/examples/device-report-power.json' ':!protocol/examples/pairing-package.json' ':!protocol/schemas/agent-config.schema.json'
```

输出为空，就表示这些文件与归档逐字节相同。

已按规则 K7 单独提交改动的基线文件（所以上面的命令不含它们）：

| 文件 | 改动 |
| --- | --- |
| `firmware/components/board_support/`、`firmware/tests/board_cache_test.cpp` | 读取电池电压、USB 电压、芯片温度，并加入电源芯片写入白名单（规则 R3），附 18 条失败模式的测试 |
| `firmware/tests/CMakeLists.txt` | 加入新测试目标 `ui_view_models`、`power_report_wire_format`、`hotspot_pairing_logic`，其余目标不变 |
| `firmware/CMakeLists.txt` | 版本号 0.2.6 改为 0.3.0，再改为 0.3.1、0.3.2、0.3.3、0.3.4，各两处 |
| `firmware/components/management/`（`management.cpp`、`CMakeLists.txt`）、`firmware/main/app_main.cpp` | 设备上报和 USB 状态带电源对象；新增纯函数 `power_wire.cpp/.hpp` 和 `firmware/tests/power_wire_test.cpp` |
| `agent/src/deskhub_agent/management_models.py`、`management.py`；`protocol/`（`openapi.json`、`schemas/device-report.schema.json`、`export_openapi.py`、`README.md`、新示例） | Agent 接收并校验电源读数，设备记录带 `power_state`（未上报、新鲜、过期）；新增 `agent/tests/test_power_report.py` |
| `agent/src/deskhub_agent/models.py`、`management.py`、`app.py`；`protocol/`（`schemas/agent-config.schema.json`、新示例 `examples/pairing-package.json`） | 热点配网的 Agent 端：Settings 新增 `public_url` 和 `pairing_ca`；新增 `pairing.py`（授权、配对包、兑换）和 `agent/tests/test_pairing_grants.py` |
| `agent/src/deskhub_agent/pairing.py`；`protocol/`（`openapi.json`、`export_openapi.py`、`README.md`、`examples/pairing-package.json`） | 配对包升到版本 2，直接带 CA 证书，删除 `GET /api/v1/pairing/ca`：固件里不留不验证的 TLS 连接，也不改 `sdkconfig.defaults`（见 `docs/pairing-design.md` 第 11 节）；向量重新生成，`export_openapi.py` 的实现校验同步 |
| `tools/provision.py` | `--status` 只打印检查过的电源对象；新增 `tools/tests/test_provision_power.py` |
| `firmware/components/board_support/`（`board.cpp`、`board.hpp`）、`firmware/tests/board_cache_test.cpp` | 充电保护用的充电开关：`board_charge_enabled`、`board_set_charging`。只写 0x18 位 1（R3 的白名单行），先认芯片型号，读出整个字节只改这一位，写后读回；采样代码仍然从不写它。新增 C1 到 C10 十组失败模式测试 |
| `firmware/tests/CMakeLists.txt`、`firmware/tests/ui_preview/`（`CMakeLists.txt`、`harness.cpp`）、`firmware/tests/ui_model_test.cpp` | 新增测试目标 `charge_test`（`charge_protection_logic`）；界面测试加入充电保护的视图和流程（T19、A16） |
| `firmware/main/`（`app_main.cpp`、`CMakeLists.txt`） | 启动时 `charge::init()`，每 250 毫秒 `charge::tick()`；能力 `charge_limit` 报实验（`unverified`）；USB 状态加只读的 `charge` 对象；常驻任务的栈从 4096 加到 6144 |
| `tools/provision.py` | `--status` 另外打印检查过的 `charge` 对象；新增 `tools/tests/test_provision_charge.py` |
| `agent/src/deskhub_agent/`（`management.py`、`management_models.py`、`cli.py`）、`agent/tests/`（`conftest.py`、`test_management.py`、`test_pairing_grants.py`、`test_power_report.py`）、`protocol/`（`openapi.json`、`export_openapi.py`、`README.md`） | 管理员密码登录（R8）：设置会话、密码会话、脚本会话；`GET /admin/auth`、`POST /admin/login`、`PUT /admin/password`；`deskhubctl admin-password-reset`。现有测试改成真实的起步方式（口令、设置密码、再用密码）。新增 `admin_auth.py` 和 `agent/tests/test_admin_password.py` |
| `firmware/components/core/`（`core.cpp`、`core.hpp`） | `network_hold`、`network_wifi_started`：热点配对期间 Wi-Fi 任务不抢无线电 |
| `firmware/tests/CMakeLists.txt` | 新增测试目标 `pairing_glue_test`（`hotspot_pairing_glue`） |
| `tools/create_tls.py`、`tools/tests/test_create_tls_reuse.py` | `--ca-dir`：沿用已有的 CA 签发新的服务器证书（服务器证书只有 397 天，到期后设备会全部拒绝 Agent；原来每次生成新 CA，只能重新配对）。旧目录只读；CA 不到 30 天过期、已过期、不是 CA、私钥不配、私钥可被别人读、符号链接、加密的私钥等都拒绝且什么都不写 |
| `firmware/main/`（`app_main.cpp`、`CMakeLists.txt`）、`tools/provision.py` | 启动 `pairing_device`；USB 状态加 `pairing` 对象（`open`、`phase`、`why`）；`--status` 只打印检查过的 `pairing` 对象；新增 `tools/tests/test_provision_pairing.py` |
| `scripts/package_firmware.py` | 只改 USB 包里 README.txt 的功能入口和验证边界文字，打包逻辑不变 |

`firmware/tests/` 下新增的 `ui_model_test.cpp` 和 `ui_preview/` 是新界面自己的测试，归档里没有，所以上面的命令也不含它们。

## 文档

- [产品范围](product-scope.md)：做什么、不做什么、每个功能的状态，以及 14 条决策记录。
- [规则](rules.md)：保留的安全规则，以及放宽的规则和用来代替的保护。
- [设计稿](design/README.md)：设计稿源文件、画板清单和设计变量。
- [配对接口设计](pairing-design.md)：热点配网（R7）的设计、实现和第 13 节的 K1 复审。
- [管理员登录的设计](admin-login-design.md)、[网页管理台](web-console-design.md)。

## 现状（开发记录）

这是按时间写下来的详细记录；给使用者看的简表在 [README](../README.md)。

- 有：Agent 的 API 和网页管理台（`/manage/`）、固件的核心、板级、管理、Agent 客户端和 USB 协议、签名与配对工具、测试。
- 有：新设备界面（`firmware/components/ui`）。8 个主要页面（总览、动作、电脑、设置、电池与电源三个分段、连接、外观）、显示与亮度、桌面时钟、设备信息三个功能页，以及配网页、离线状态、充电保护确认三个状态页。固件可以完整构建（0 个编译警告，镜像约 4.4 MiB，应用分区 6 MiB）。
- 界面验证只在电脑上做过：模型测试和原生 LVGL 离屏预览（见下面的命令）。**没有在真机上验证过**：屏幕、触摸、电源读数、常亮都还没有实测。
- 顶栏改成图标（设计稿画板 `Dev-Header`，2026-10-29）：Wi-Fi、电脑、USB、闪电、电池，数字写在电池里（不带 %，可在外观里隐藏）。正在连接的图标闪烁（`blink_on`，每秒一次）。电池里的数字压在 40% 强度的填充上，数字对比度在三个主题里都不低于 4.5:1（`kBatteryFillPercent`，测试 T20）。外观里多了“电池百分比”和“圆环粗细”（细、标准、粗：总览的状态环 12、20、28 px，电量页的仪表 7、11、15 px）。外观的存储格式多了两个字段，0.3.x 存的四个字段仍然能读。屏幕上不再有“实验功能”字样（规则 R1 已改）。
- 屏幕是圆角的，Waveshare 没公布半径。界面用 `kCornerR`（现在是 80 像素）和 `corner_inset()` 躲开四角；预览测试 A17 检查每页、每个主题、语言和字号。半径是猜的，真机上是否还有被切的，要看过才知道。
- 配网页现在由 `pairing_device` 调用（长按 GPIO0 或未配对时开机 8 秒后）；没有在真机上看过。首页没有动作时不再画“设置网络与配对”卡片（2026-10-29）。
- 有：充电保护（N1，实验功能）的完整实现，设计见 [docs/charge-protection-design.md](charge-protection-design.md)：策略和失败方向、电源芯片充电开关的读写（只写 0x18 位 1）、保存、界面（保护页、确认框、状态文字）、USB 状态里的 `charge` 对象。主机测试、界面预览和变异检查（`scripts/mutation/`）都通过。**还没有刷到设备，没有在真机上停过充电**；电量计的估算也没对照过这颗电芯。刷旧固件之前要先关闭它，见 [docs/flashing.md](flashing.md)。
- 有：电源读数的完整数据通路。固件把电量、电池电压、USB 电压、芯片温度和测量状态放进设备上报和 USB 状态；Agent 校验后保存，管理接口的设备记录带新鲜度；`tools/provision.py --status` 显示检查过的读数。固件这一侧已随 0.3.1 刷到设备：USB 状态里读到了电源对象（电池 4183 mV、USB 5156 mV、芯片温度 40.3 °C、电量 100%、已充满）。读数只读了一次，没有和仪表对照；设备没有配置 Wi-Fi 和 Agent，所以设备到 Agent 的上报没有在真机上试过。
- 有：热点配网的 Agent 端（授权、配对包版本 2 带 CA、无凭据兑换；没有 CA 下载接口）和设备端的纯逻辑（配对包解码、会话状态机、输入校验、倒计时、长按），都有主机测试和变异检查；设备配网页有“热点密码”一行。
- 设备端的接线做完了（2026-10-29）：热点、配网页、TLS 兑换、保存路径、按键，`firmware/components/pairing_device/`；能在主机上测的部分在 `pairing_glue`（测试 `pairing_glue_test`，变异检查 46 处全部报红）。K1 复审在 [配对接口设计](pairing-design.md) 第 13 节。固件完整构建通过。**没有在真机上验证任何一步**：按键引脚（GPIO0）没有核对原理图，热点与 Wi-Fi 同信道、堆内存、手机能否打开配网页都要看真机。USB 状态的 `pairing` 对象（`tools/provision.py --status`）用来读它做到哪一步。
- 有：管理员密码登录（R8，`admin_auth.py`：scrypt、锁定、命令行重置，40 条测试，64 处变异全部报红）和网页管理台（`agent/src/deskhub_agent/web/`，九个页面）。管理台用真实的 Agent 加真实的 Chrome 走过整套流程（`scripts/e2e/console.e2e.mjs`，62 项通过，证据在 `docs/evidence/console-e2e/`）；没有接真实设备和显示器。
- 没有：声音（N2 要先确认扬声器是否接在板上）；网页上开关充电保护（设备的上报里没有这一项）。
- 固件版本 0.3.6（2026-10-29）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。比 0.3.4 多：电池数字居中、首页圆环加说明（点圆环看四段的意思）、设备端热点配网接线（没有真机验证）、配网时屏幕不熄灭、首页没有动作时不画设置卡片、USB 状态的 pairing 对象。**还没有刷到任何设备**。不生成签名 OTA 包。记录在 [docs/evidence/firmware-0.3.6.json](evidence/firmware-0.3.6.json)；0.3.5 从未刷到设备，已被它取代（[firmware-0.3.5.json](evidence/firmware-0.3.5.json)）。
- 上一个版本 0.3.4（图标顶栏、电池里写百分比、圆环粗细可调、没有“实验功能”字样）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。**还没有刷到任何设备**。不生成签名 OTA 包（你决定不需要）。哈希和做过的检查记在 [docs/evidence/firmware-0.3.4.json](evidence/firmware-0.3.4.json)。上一个版本 0.3.3 的记录在 [docs/evidence/firmware-0.3.3.json](evidence/firmware-0.3.3.json)。
- 上一个版本 0.3.1 已用 USB 包刷到设备：写入校验通过，开机后界面心跳正常，USB 状态里读到了电源数据；刷完以后没有人看过屏幕。记录在 [docs/evidence/firmware-0.3.1.json](evidence/firmware-0.3.1.json)。
- 上一个版本 0.3.0 的记录在 [docs/evidence/firmware-0.3.0.json](evidence/firmware-0.3.0.json)。0.3.0 刷入后用户在设备旁确认“屏幕显示正常，新界面已经出来”；那次也没有逐页检查。

运行基线测试：

```sh
.venv/bin/python -m pytest agent/tests -q
.venv/bin/python -m unittest discover -s tools/tests
.venv/bin/python protocol/export_openapi.py --check
node scripts/e2e/console.e2e.mjs   # 网页管理台：真实的 Agent 加 Chrome，约 2 分钟，证据写到 docs/evidence/console-e2e/
cmake -S firmware/tests -B firmware/build-host && cmake --build firmware/build-host && ctest --test-dir firmware/build-host --output-on-failure
python3 firmware/tests/ui_preview/render.py
```

最后一条构建真实的界面代码，在原生 LVGL 里离屏运行 15 组检查，同时运行触摸方向回归测试，并把各页面、各主题的截图存到 `firmware/tests/ui_preview/out/`。结果全部是模拟数据。

变异检查（故意改坏实现，看测试会不会报红）：`scripts/mutation/` 下有配对（Agent 端、设备端、设备端接线的纯逻辑）、充电保护（策略、界面）、屏幕圆角、顶栏图标、圆环说明、管理员密码、证书续期和网页管理台。用法和输出的含义见 [scripts/mutation/README.md](../scripts/mutation/README.md)。

完整构建固件（独立目录，不碰别的构建）：

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
unset IDF_COMPONENT_LOCAL_STORAGE_URL
cd firmware && idf.py -B build-0.3.4 build
```

## 不在 git 里的东西

`.state/` 存放签名私钥、管理员凭据、CA 与 TLS 私钥、Agent 数据库和原厂 Flash 备份。它被 `.gitignore` 排除，**不要提交，也不要复制进任何分支**。
`dist/`、固件构建目录、`firmware/managed_components/` 同样被忽略。
