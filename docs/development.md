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
git diff --stat archive/v0.2 main -- tools protocol firmware/components/core firmware/components/management firmware/components/agent_client firmware/main firmware/tests ':!firmware/tests/board_cache_test.cpp' ':!firmware/tests/CMakeLists.txt' ':!firmware/tests/ui_model_test.cpp' ':!firmware/tests/ui_preview' ':!firmware/tests/power_wire_test.cpp' ':!firmware/tests/pairing_test.cpp' ':!firmware/tests/charge_test.cpp' ':!firmware/main/CMakeLists.txt' ':!firmware/components/management/management.cpp' ':!firmware/components/management/CMakeLists.txt' ':!firmware/components/management/power_wire.cpp' ':!firmware/components/management/include/power_wire.hpp' ':!firmware/main/app_main.cpp' ':!tools/provision.py' ':!tools/tests/test_provision_power.py' ':!tools/tests/test_provision_charge.py' ':!protocol/openapi.json' ':!protocol/schemas/device-report.schema.json' ':!protocol/export_openapi.py' ':!protocol/README.md' ':!protocol/examples/device-report-power.json' ':!protocol/examples/pairing-package.json' ':!protocol/schemas/agent-config.schema.json' ':!firmware/components/core/core.cpp' ':!firmware/components/core/include/core.hpp' ':!firmware/tests/pairing_glue_test.cpp' ':!firmware/tests/audio_test.cpp' ':!tools/create_tls.py' ':!tools/tests/test_create_tls_reuse.py' ':!tools/tests/test_provision_pairing.py' ':!tools/tests/test_provision_sound.py' ':!tools/tests/test_provision_wifi_change.py' ':!firmware/tests/display_transfer_test.cpp' ':!firmware/tests/keyboard_test.cpp' ':!firmware/tests/wifi_change_test.cpp' ':!firmware/tests/board_stubs/driver/spi_common.h' ':!firmware/tests/board_stubs/esp_lv_adapter.h' ':!firmware/components/core/policy.cpp' ':!firmware/components/core/include/policy.hpp' ':!firmware/tests/display_buffers_test.cpp' ':!firmware/tests/ble_protocol_test.cpp' ':!firmware/tests/ble_update_test.cpp' ':!firmware/tests/board_stubs/esp_lv_adapter_display.h'
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
| `firmware/tests/CMakeLists.txt`、`firmware/tests/audio_test.cpp` | 新增测试目标 `audio_test`（`sound_rules`）：提示音的波形、命令文字对应的音、什么时候可以响 |
| `firmware/tests/`（`CMakeLists.txt`、`ui_model_test.cpp`、`ui_preview/CMakeLists.txt`、`ui_preview/harness.cpp`） | `ui_model_test` 带上 `policy.cpp`，加 T22（夜间不响）；界面预览带上 `audio_policy.cpp` 和一个假的扬声器，加声音页、闪电在电池里、没有动作时的首页三组检查 |
| `firmware/main/`（`app_main.cpp`、`CMakeLists.txt`）、`tools/provision.py`、`tools/tests/test_provision_sound.py` | 启动 `audio::init()`；能力 `audio` 按声音芯片是否应答报 `unverified` 加 ready 或 unavailable；USB 命令 `sound_test`（只回三个布尔值）；`--sound-test` 不能和别的操作混用，只打印检查过的三个值 |
| `firmware/tests/audio_test.cpp`、`firmware/tests/ui_preview/harness.cpp` | 音量三档：S5（存储只认“0”“1”“2”、默认低、每档至少差 6 dB、最高不到芯片 0 dB）；界面预览的声音组加音量的点选、存不下、没有扬声器 |
| `firmware/components/core/`、`firmware/components/management/management.cpp` | 无线独占（NetworkOwner），配对、换 Wi-Fi、配置写入和 OTA 互斥；`save_wifi_credentials` 只改名称和密码；只有 Wi-Fi、没有 Agent 的凭据也能读回 |
| `firmware/components/board_support/`、`firmware/tests/board_cache_test.cpp`、`firmware/tests/display_transfer_test.cpp`、`firmware/tests/board_stubs/` | 修显示横纹：LCD 的 SPI DMA 每块不超过 4092 字节；开机的旋转和亮度命令先拿显示锁（`docs/evidence/display-corruption.md`） |
| `firmware/main/`、`tools/provision.py`、`tools/tests/test_provision_wifi_change.py` | USB 状态加只读的 `wifi_change`；无线被占用时 USB 配对回 BUSY |
| `firmware/tests/`（`CMakeLists.txt`、`keyboard_test.cpp`、`wifi_change_test.cpp`、`ui_preview/`） | 键盘、换网状态机和触屏流程的测试 |
| `firmware/components/core/`（`core.cpp`、`policy.cpp`、`policy.hpp`）、`firmware/tests/ui_model_test.cpp`、`firmware/tests/ui_preview/` | 时区：配置多一个 `tz_minutes`（只在设备上，不发给 Agent），开机、保存、恢复出厂时设置 TZ；T23、A29 |
| `firmware/main/`（`app_main.cpp`、`CMakeLists.txt`） | 调用测试探针 `probe_start_early()`、`probe_start()`（`components/probe`）。正式版配置里探针关闭，什么也不做；只有 `sdkconfig.probe_*` 的测试构建打开（E2，`docs/evidence/ble-memory.json`） |
| `firmware/tests/ui_preview/harness.cpp` | 恢复出厂的 F1 到 F4：声音和外观也被清掉，删不掉时要说 |
| `scripts/ui_preview/touch_orientation_test.cpp` | 补上显示锁的替身：0.3.13 以后 `render.py` 整条命令链接失败，这个回归一直没有运行 |
| `firmware/components/board_support/`（`display_buffers.cpp`、`board.cpp`、`CMakeLists.txt`）、`firmware/tests/`（`display_buffers_test.cpp`、`board_cache_test.cpp`、`board_stubs/esp_lv_adapter_display.h`、`CMakeLists.txt`）、`scripts/ui_preview/touch_orientation_test.cpp` | 绘图缓冲开机预留在内部内存（拦截 `esp_lv_adapter_register_display`），LVGL 任务栈 12 KB 放 PSRAM；去掉横纹的根源 |
| `firmware/tests/ui_model_test.cpp`、`firmware/tests/ui_preview/`（`harness.cpp`、`CMakeLists.txt`） | 界面 v2：页面与首页、切换页样式、输入源名字和图标、夜航主题，去掉电脑指标（T24 到 T27、A28 到 A34） |
| `firmware/sdkconfig.defaults`、`firmware/main/`（`app_main.cpp`、`CMakeLists.txt`）、`firmware/components/board_support/`（`lvgl_psram.c`、`CMakeLists.txt`） | 蓝牙连 App（0.3.18）：NimBLE 和 H2 的内存布局进正式配置；蓝牙最先启动；LVGL 分配器从测试探针移到 board_support；USB 和电源任务栈放 PSRAM，USB 请求交给 flash_worker，任务起不来时写进状态 |
| `firmware/tests/`（`ble_protocol_test.cpp`、`CMakeLists.txt`、`ui_preview/`） | 蓝牙协议 B1 到 B6；界面 A35 到 A39 |
| `firmware/tests/`（`ui_model_test.cpp`、`ui_preview/harness.cpp`） | 秒和表针可选（0.3.19）：外观文字第 9 段的 T5、T25；界面 A40；时间页的“手动校时”检查改在大数字下做 |
| `firmware/components/core/`（`core.cpp`、`core.hpp`）、`firmware/components/management/`（`management.cpp`、`management.hpp`）、`firmware/main/app_main.cpp` | 蓝牙升级（0.3.22，docs/desktop-app-design.md 13.11）：core 只存更新公钥（`save_update_key`，同一个 NVS 位置）；management 对外给出“能不能升级”和签名清单核对（和 Agent 那条路同一套规则），蓝牙升级进行时 Agent 的升级等着；USB 新请求 `update_key`；main 把这些挂到蓝牙模块上 |
| `firmware/tests/`（`ble_update_test.cpp`、`ble_protocol_test.cpp`、`CMakeLists.txt`） | 蓝牙升级 O1 到 O9；蓝牙信息里的公钥指纹 |
| `firmware/tests/`（`ble_protocol_test.cpp`、`ui_model_test.cpp`、`CMakeLists.txt`、`ui_preview/harness.cpp`） | 用量页（0.3.28）：B7 消息解析；T28 第 5 个页面、T29 用量页文字；A42 用量页；旧测试改成 5 个页面 |
| `firmware/components/management/`（`management.cpp`、`include/management.hpp`、`CMakeLists.txt`）、`firmware/main/app_main.cpp` | 0.3.29：开机自检不再依赖管理任务（13.11 U16、U17）。开机时在主任务里读“待确认”；电源任务每 250 毫秒调用 `management_tick()`，自检在 flash 工作任务上跑；管理任务建不成时打日志、在状态里说明；蓝牙 info 的 `boot` 字段由 `boot_pending()` 提供 |
| `firmware/tests/ble_protocol_test.cpp` | info 消息多了 `boot` 字段（`pending` / `confirmed`）；0.3.30：`boot_pending_now` 的四种组合（钩子还没装上时按开机时读到的状态，13.11 U18） |
| `firmware/tests/ble_protocol_test.cpp`、`firmware/tests/ui_model_test.cpp`、`firmware/tests/ui_preview/harness.cpp` | 0.3.31 待办看板（13.13）：B8 到 B10（消息、成批收、一次一个操作）；T30 到 T32（第 6 个页面和旧设置的迁移、列表的文字、字库没有的字），旧的页面设置期望值多了 `T-`；A43（选中、长按、删除确认和自动取消、过期、关掉、英文），界面矩阵多一页；角上小标记的检查和文字一样，跳过滚出主体的部分 |
| `firmware/tests/ble_protocol_test.cpp`、`firmware/tests/ui_model_test.cpp`、`firmware/tests/audio_test.cpp`、`firmware/tests/ui_preview/harness.cpp` | 0.3.32 待办的更改和响铃（13.14）：B11、B12（新字段、清单、`todo_edit`）；T33、T34（重复的文字、月份波轮、草稿、响铃的判断）；S7（三声的响铃）；A44（更改页、波轮、重复、清单、保存和取消、响铃页、10 分钟后再响、夜间不响），界面测试可以设墙上时间 |
| `firmware/tests/ui_model_test.cpp`、`firmware/tests/ui_preview/harness.cpp` | 0.3.33 设计评审以后（13.15）：T35（星期几、到期页标题、响铃页下面一行）；A45（完成后 5 秒可撤销 N1 到 N13）；A43、A44 的“完成”改成 5 秒后才发 |
| `firmware/components/core/`（删 `config_jobs.cpp`；`core.cpp`、`core.hpp`、`CMakeLists.txt`） | 代码评审（2026-11-12）：配置写入的后台任务从来没人用（`queue_config_save`、`queue_config_reset`、`config_job_result`、`config_draft`、带草稿的 `reset_config` 都没有调用方），却在开机时建一个 6 KB 内部内存栈的 `dh_config` 任务。删掉，内部内存留给蓝牙。从 v0.2 归档原样带来的 |
| `firmware/components/board_support/`（删 `touch_input.cpp`、`touch_input.hpp`、`touch_sample_queue.hpp`；`CMakeLists.txt`） | 代码评审（2026-11-12）：自己的触摸采样模块，`touch_input_start()` 从来没被调用（触摸走 BSP 的 LVGL 输入设备），因为 `WHOLE_ARCHIVE` 仍然链接进固件。删掉。从 v0.2 归档原样带来的 |
| `firmware/CMakeLists.txt` | 版本号改为 0.3.5 到 0.3.33，各两处 |

`firmware/tests/` 下新增的 `ui_model_test.cpp` 和 `ui_preview/` 是新界面自己的测试，归档里没有，所以上面的命令也不含它们。

## 文档

- [产品范围](product-scope.md)：做什么、不做什么、每个功能的状态，以及 14 条决策记录。
- [规则](rules.md)：保留的安全规则，以及放宽的规则和用来代替的保护。
- [设计稿](design/README.md)：设计稿源文件、画板清单和设计变量。
- [配对接口设计](pairing-design.md)：热点配网（R7）的设计、实现和第 13 节的 K1 复审。
- [管理员登录的设计](admin-login-design.md)、[网页管理台](web-console-design.md)。
- [桌面 App 设计](desktop-app-design.md)：菜单栏 App 和 DDC 切源（N4，设计中）。

## 现状（开发记录）

这是按时间写下来的详细记录；给使用者看的简表在 [README](../README.md)。

- 有：Agent 的 API 和网页管理台（`/manage/`）、固件的核心、板级、管理、Agent 客户端和 USB 协议、签名与配对工具、测试。
- 有：新设备界面（`firmware/components/ui`）。8 个主要页面（总览、动作、电脑、设置、电池与电源三个分段、连接、外观）、显示与亮度、桌面时钟、设备信息三个功能页，以及配网页、离线状态、充电保护确认三个状态页。固件可以完整构建（0 个编译警告，镜像约 4.4 MiB，应用分区 6 MiB）。
- 界面验证只在电脑上做过：模型测试和原生 LVGL 离屏预览（见下面的命令）。**没有在真机上验证过**：屏幕、触摸、电源读数、常亮都还没有实测。
- 顶栏改成图标（设计稿画板 `Dev-Header`，2026-10-29）：Wi-Fi、电脑、USB、闪电、电池，数字写在电池里（不带 %，可在外观里隐藏）。正在连接的图标闪烁（`blink_on`，每秒一次）。电池里的数字压在 40% 强度的填充上，数字对比度在三个主题里都不低于 4.5:1（`kBatteryFillPercent`，测试 T20）。外观里多了“电池百分比”和“圆环粗细”（细、标准、粗：总览的状态环 12、20、28 px，电量页的仪表 7、11、15 px）。外观的存储格式多了两个字段，0.3.x 存的四个字段仍然能读。屏幕上不再有“实验功能”字样（规则 R1 已改）。
- 屏幕是圆角的，Waveshare 没公布半径。界面用 `kCornerR`（现在是 80 像素）和 `corner_inset()` 躲开四角；预览测试 A17 检查每页、每个主题、语言和字号。半径是猜的，真机上是否还有被切的，要看过才知道。
- 配网页现在由 `pairing_device` 调用（长按 GPIO0 或未配对时开机 8 秒后）；没有在真机上看过。首页没有动作时不再画“设置网络与配对”卡片（2026-10-29）。
- 有：充电保护（N1，实验功能）的完整实现，设计见 [docs/charge-protection-design.md](charge-protection-design.md)：策略和失败方向、电源芯片充电开关的读写（只写 0x18 位 1）、保存、界面（保护页、确认框、状态文字）、USB 状态里的 `charge` 对象。主机测试、界面预览和变异检查（`scripts/mutation/`）都通过。0.3.6 刷到设备后，开机读到它在 90% 上限处停充，写入和读回都是 ok（设备自己报告，没有用仪表对照，见 [firmware-0.3.6.json](evidence/firmware-0.3.6.json)）。**降到回充线以下再恢复充电还没有验证**；电量计的估算也没对照过这颗电芯。刷旧固件之前要先关闭它，见 [docs/flashing.md](flashing.md)。
- 有：电源读数的完整数据通路。固件把电量、电池电压、USB 电压、芯片温度和测量状态放进设备上报和 USB 状态；Agent 校验后保存，管理接口的设备记录带新鲜度；`tools/provision.py --status` 显示检查过的读数。固件这一侧已随 0.3.1 刷到设备：USB 状态里读到了电源对象（电池 4183 mV、USB 5156 mV、芯片温度 40.3 °C、电量 100%、已充满）。读数只读了一次，没有和仪表对照；设备没有配置 Wi-Fi 和 Agent，所以设备到 Agent 的上报没有在真机上试过。
- 有：热点配网的 Agent 端（授权、配对包版本 2 带 CA、无凭据兑换；没有 CA 下载接口）和设备端的纯逻辑（配对包解码、会话状态机、输入校验、倒计时、长按），都有主机测试和变异检查；设备配网页有“热点密码”一行。
- 设备端的接线做完了（2026-10-29）：热点、配网页、TLS 兑换、保存路径、按键，`firmware/components/pairing_device/`；能在主机上测的部分在 `pairing_glue`（测试 `pairing_glue_test`，变异检查 46 处全部报红）。K1 复审在 [配对接口设计](pairing-design.md) 第 13 节。固件完整构建通过。**没有在真机上验证任何一步**：按键引脚（GPIO0）没有核对原理图，热点与 Wi-Fi 同信道、堆内存、手机能否打开配网页都要看真机。USB 状态的 `pairing` 对象（`tools/provision.py --status`）用来读它做到哪一步。
- 有：管理员密码登录（R8，`admin_auth.py`：scrypt、锁定、命令行重置，40 条测试，64 处变异全部报红）和网页管理台（`agent/src/deskhub_agent/web/`，九个页面）。管理台用真实的 Agent 加真实的 Chrome 走过整套流程（`scripts/e2e/console.e2e.mjs`，62 项通过，证据在 `docs/evidence/console-e2e/`）；没有接真实设备和显示器。
- 有：声音（N2，2026-10-30，0.3.7）。你确认板上有扬声器。规则在 `firmware/components/audio/audio_policy`（主机测试 `audio_test`），扬声器在 `audio.cpp`（板子的声音芯片、一个队列、NVS 里的开关）。设计稿 `Dev-Sound`：设置里多一行“声音”，提示音开关默认关，试听，没有扬声器时变暗。动作有结果（`ui.cpp` 看到新的命令文字）、热点配对完成时响；夜间限亮时段不响。USB 命令 `sound_test` 和 `tools/provision.py --sound-test`。变异检查 `audio_policy.py` 33 处、`sound_ui.py` 22 处全部报红。**没有人听过**：芯片应答只说明它在，不说明喇叭响了、响度合适。
- 声音的音量三档（2026-10-31，0.3.8）：你听到声音以后要音量可调。低、中、高是声音芯片的音量 50、70、90（芯片曲线 0.5 dB 一级，每档差 10 dB），默认低就是 0.3.7 的固定音量；存在 NVS 的 `dh_snd/vol`，只认“0”“1”“2”。点一档马上存下并响一声试听音。测试 S5 和界面预览的声音组；变异检查 `audio_policy.py` 42 处、`sound_ui.py` 32 处全部报红。设计稿 `Dev-Sound` 已改；设计画布打不开，只改了仓库里的画板。
- 顶栏的闪电挪进电池里，画在数字前面，和数字一起居中；USB 紧挨电池（`Dev-Header`）。没有动作时总览的圆环放大到 252 px 并上下居中（`Dev-HomeEmpty`）。变异检查 `header_home_ui.py` 20 处全部报红。
- 没有：网页上开关充电保护（设备的上报里没有这一项）。
- 固件版本 0.3.16（2026-11-07）：已构建并打包，**还没有刷到设备**。比 0.3.15 多：屏幕绘图缓冲开机预留在内部内存，发送时不再临时中转（横纹的根源）；LVGL 任务栈 12 KB 放 PSRAM。蓝牙仍然关闭。记录在 [docs/evidence/firmware-0.3.16.json](evidence/firmware-0.3.16.json)。
- 上一个版本 0.3.15（2026-11-06）：比 0.3.14 多一处修复：恢复出厂也清掉声音和外观设置。2026-11-06 已刷到设备：开机没有自动开热点，Wi-Fi 连续 150 秒都是已连接（设备自报）；屏幕没有人看过。记录在 [docs/evidence/firmware-0.3.15.json](evidence/firmware-0.3.15.json)。
- 0.3.14（2026-11-05）：已构建并打包，从没刷到设备，已被 0.3.15 取代。比 0.3.13 多：已存 Wi-Fi 的设备开机不再自动开热点（R7 第 1 条）；屏幕上开始配对；换 Wi-Fi 的结果页说真实的结果（W17 到 W22）；连接页写 Wi-Fi 名称和信号；键盘提示跟着输入变；列表用信号格；时区和手动校时（`Dev-Time`）。记录在 [docs/evidence/firmware-0.3.14.json](evidence/firmware-0.3.14.json)。
- 上一个版本 0.3.13（2026-11-03）：已刷到设备，你确认显示正常。比 0.3.8 多：屏幕上换 Wi-Fi（软键盘）、显示横纹的修复。中间版本 0.3.9 到 0.3.12 都是调查用的。记录在 [docs/evidence/firmware-0.3.13.json](evidence/firmware-0.3.13.json) 和 [display-corruption.md](evidence/display-corruption.md)。
- 上一个版本 0.3.8（2026-10-31）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。比 0.3.7 多：声音的音量三档。2026-11-02 核对：设备上已是 0.3.8，应用镜像与包里的逐字节相同（不是我刷的），USB 状态正常、充电保护仍在上限处停充。三档的响度没有人听过；屏幕没有看过。记录在 [docs/evidence/firmware-0.3.8.json](evidence/firmware-0.3.8.json)。
- 上一个版本 0.3.7（2026-10-30）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。比 0.3.6 多：声音（提示音、试听、`sound_test`）、闪电在电池里、没有动作时首页圆环放大居中。2026-10-30 已刷到设备：USB 状态里能力 audio 是 ready（声音芯片应答了），充电保护仍在上限处停充。2026-10-31 你在设备旁听到了试听音。响度、动作和配对触发的提示音还没有试；屏幕没有看过。记录在 [docs/evidence/firmware-0.3.7.json](evidence/firmware-0.3.7.json)。
- 上一个版本 0.3.6（2026-10-29）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。比 0.3.4 多：电池数字居中、首页圆环加说明（点圆环看四段的意思）、设备端热点配网接线（没有真机验证）、配网时屏幕不熄灭、首页没有动作时不画设置卡片、USB 状态的 pairing 对象。2026-10-29 已刷到设备，读到了 USB 状态（热点自动打开、充电保护停充；屏幕和手机没有看过）。不生成签名 OTA 包。记录在 [docs/evidence/firmware-0.3.6.json](evidence/firmware-0.3.6.json)；0.3.5 从未刷到设备，已被它取代（[firmware-0.3.5.json](evidence/firmware-0.3.5.json)）。
- 上一个版本 0.3.4（图标顶栏、电池里写百分比、圆环粗细可调、没有“实验功能”字样）：已构建（0 个警告）并打成 USB 刷机包，包在 `dist/`（不在 git 里），离线校验通过。**还没有刷到任何设备**。不生成签名 OTA 包（你决定不需要）。哈希和做过的检查记在 [docs/evidence/firmware-0.3.4.json](evidence/firmware-0.3.4.json)。上一个版本 0.3.3 的记录在 [docs/evidence/firmware-0.3.3.json](evidence/firmware-0.3.3.json)。
- 上一个版本 0.3.1 已用 USB 包刷到设备：写入校验通过，开机后界面心跳正常，USB 状态里读到了电源数据；刷完以后没有人看过屏幕。记录在 [docs/evidence/firmware-0.3.1.json](evidence/firmware-0.3.1.json)。
- 上一个版本 0.3.0 的记录在 [docs/evidence/firmware-0.3.0.json](evidence/firmware-0.3.0.json)。0.3.0 刷入后用户在设备旁确认“屏幕显示正常，新界面已经出来”；那次也没有逐页检查。

### 屏幕软键盘换 Wi-Fi（2026-11-03，0.3.13 起）

实现位置：`firmware/components/wifi_change/` 管理扫描、试连、检查 Agent、确认和超时回退；`ui/ui_wifi.cpp` 和 `ui/include/ui_keyboard.hpp` 提供全屏软键盘。输入名称、大小写和全部 ASCII 符号；密码仅在按住眼睛键时显示。键盘页退出、取消触摸和息屏都会隐藏密码。

安全基线的这批改动已按 K7 单独提交（2026-11-05，`ef1065d` 到 `49bf5e3`）：

| 文件 | 原因与验证 |
| --- | --- |
| `firmware/components/core/core.cpp`、`include/core.hpp` | 无线独占与配置写入互斥；临时 Wi-Fi 凭据只在 RAM 中，验证成功后复用双槽 NVS 保存；保留 Agent 地址、CA、令牌和升级公钥；独占期间拒绝配置、重置和 OTA。状态机先写失败用例，真实驱动与 NVS 断电行为尚未实测 |
| `firmware/components/management/management.cpp` | 试连期间暂停管理请求，并在开始 OTA 前原子检查无线所有权，防止在途回复启动更新 |
| `firmware/main/app_main.cpp`、`main/CMakeLists.txt` | 初始化换网组件，USB 状态加白名单诊断 `wifi_change`，配对和换网互斥 |
| `tools/provision.py`、`tools/tests/test_provision_wifi_change.py` | 过滤新状态中的阶段、原因、时间和布尔值；不回显名称、密码或未知字段，先写失败用例 |
| `firmware/tests/CMakeLists.txt`、`keyboard_test.cpp`、`wifi_change_test.cpp` | 新增键盘输入和换网失败场景；其余目标保留 |
| `firmware/tests/ui_preview/` | 新增真实 LVGL 触屏流程，Wi-Fi 为替身；模拟真实拖出、按住、松手、重建和息屏。测试脚本支持 `DESKHUB_PREVIEW_CASE` 只跑指定组 |

验证结果：15 组固件主机检查、116 条工具检查、26 组原生 LVGL 流程通过。ESP-IDF v6.0.2 完整构建通过，无编译警告。键盘模型 7 处、触屏流程 4 处、换网状态机 8 处变异全部被发现。报告、截图和重复命令在 [软键盘验证证据](evidence/wifi-change/README.md)。网页管理台未改，本轮没有重跑浏览器 E2E。

实现阶段先用临时 0.3.8 构建完成主机验证。2026-11-03 用户要求“刷到设备”后，版本升为 0.3.9（`firmware/CMakeLists.txt` 两处，K7 版本基线改动），重新完整构建，无编译警告，并生成独立 USB 包 `dist/deskhub-0.3.9-esp32s3.zip`。四个镜像写入并校验成功；USB 读回 0.3.9、`ui_alive: true`，主题 ocean、旋转 180、配置版本 37 均保留。设备尚未配置 Wi-Fi 或配对 Agent；启动后配对热点打开，热点期间换网入口禁用。证据见 [0.3.9 刷机记录](evidence/firmware-0.3.9.json)。没有生成签名 OTA 包，没有提交代码。真实路由器、NVS 断电回退和触屏手感仍未验证。

### 既有显示条带对照（2026-11-03，0.3.10）

用户在 0.3.9 刷入后提供键盘照片，显示条带、缺笔和旧内容残留，并说明上一版本已有同样问题。尚未确认根因。0.3.10 只将失效区在绘制前扩展为整行，保留硬件旋转、QSPI 时钟和缓冲配置，用于对比小窗口刷新路径。`board_support/board.cpp` 与版本号属于本轮基线改动，尚未提交。先写失败方式和板级检查并确认失败，再实现回调；15 组主机检查通过。不能把该对照称为已验证修复。用户反馈所有页面仍有横纹，故撤回整行刷新和对应板级检查。0.3.11 恢复原刷新范围，并用 `display_clock.cpp` 包装 LCD SPI 创建调用，将 40 MHz 限速为 20 MHz，保留其他配置；不修改 managed_components。先写全部失败方式和检查，首次因实现文件尚不存在而编译失败，实现后 16 组主机检查通过；最终 ELF map 与反汇编确认包装调用和限速常量生效。0.3.11 真机连续报告 `Failed to allocate priv TX buffer` 和像素入队失败，降频没有消除错误。0.3.12 已移除降频包装和检查，恢复 40 MHz；`display_transfer.cpp` 改为包装 SPI2 总线初始化，将 DMA 事务限定为 4092 字节，利用 IDF LCD 驱动现有分块逻辑降低内部连续临时内存需求。先写失败方式和检查，首次因实现文件不存在而编译失败；实现后 16 组主机检查通过，完整构建无警告，ELF map 确认 BSP 调用已包装。0.3.12 真机采样中 DMA 分配错误消失，但启动命令仍有 SPI 总线冲突。0.3.13 给 `board.cpp` 中启动后的旋转、亮度命令加现有显示锁，并传播亮度失败；新增检查在修复前以 `unlocked display rotation command` 失败，修复后 16 组主机检查通过。已完整构建、打包、刷机，四个镜像校验通过，读回 0.3.13 和 ui_alive true；30 秒驱动采样未再记录此前错误。基线文件 `board.cpp`、版本和组件链接配置的修改尚未提交；2026-11-03 用户确认“显示正常”，长期稳定尚未验证。真实驱动日志和用户观察结果见 [显示问题记录](evidence/display-corruption.md)。

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

## README 的截图

- **触摸屏**：`firmware/tests/ui_preview` 的界面测试程序加一个输出目录参数，就把各个画面存成 PPM（`<build>/deskhub_ui_preview <目录>`，见 `case_screenshots`）。README 用的是其中挑出的十几张，转成 PNG 并按屏幕圆角裁好，放在 `docs/images/device/`。它们是真实界面代码的渲染，不是照片。
- **桌面 App**：`open -W -a desktop/build/DeskHub.app --args --screenshots docs/images/app --package <固件包>`。App 用真实数据把菜单和各个设置页渲染成 PNG，然后退出（`desktop/Sources/DeskHub/Screenshots.swift`）。它只读不写；不读 USB 状态（打开串口可能让触摸屏重启）。公开前把“显示器”页的 UUID、序列号和 IO 位置涂掉。
- **图**：README 里的架构图、流程图、时序图和状态图是 Mermaid，GitHub 直接渲染。改完先在浏览器里用 mermaid 11 渲染一遍，确认没有语法错误。
