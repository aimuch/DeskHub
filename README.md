# DeskHub

桌面中枢：Waveshare ESP32-S3-Touch-AMOLED-2.16 触摸屏设备、电脑端 Host Agent、网页管理台。
这个分支是**从头重新设计**的版本。

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
git diff --stat archive/v0.2 main -- tools protocol firmware/components/core firmware/components/management firmware/components/agent_client firmware/main firmware/tests ':!firmware/tests/board_cache_test.cpp' ':!firmware/tests/CMakeLists.txt' ':!firmware/tests/ui_model_test.cpp' ':!firmware/tests/ui_preview'
```

输出为空，就表示这些文件与归档逐字节相同。

已按规则 K7 单独提交改动的基线文件（所以上面的命令不含它们）：

| 文件 | 改动 |
| --- | --- |
| `firmware/components/board_support/`、`firmware/tests/board_cache_test.cpp` | 读取电池电压、USB 电压、芯片温度，并加入电源芯片写入白名单（规则 R3），附 18 条失败模式的测试 |
| `firmware/tests/CMakeLists.txt` | 加入新设备界面的模型测试目标 `ui_view_models`，其余目标不变 |
| `firmware/CMakeLists.txt` | 版本号 0.2.6 改为 0.3.0，两处 |
| `scripts/package_firmware.py` | 只改 USB 包里 README.txt 的功能入口和验证边界文字，打包逻辑不变 |

`firmware/tests/` 下新增的 `ui_model_test.cpp` 和 `ui_preview/` 是新界面自己的测试，归档里没有，所以上面的命令也不含它们。

## 文档

- [产品范围](docs/product-scope.md)：做什么、不做什么、每个功能的状态，以及 14 条决策记录。
- [规则](docs/rules.md)：保留的安全规则，以及放宽的规则和用来代替的保护。
- [设计稿](docs/design/README.md)：设计稿源文件、画板清单和设计变量。

## 现状

- 有：Agent 的 API（没有管理台）、固件的核心、板级、管理、Agent 客户端和 USB 协议、签名与配对工具、测试。
- 有：新设备界面（`firmware/components/ui`）。8 个主要页面（总览、动作、电脑、设置、电池与电源三个分段、连接、外观）、显示与亮度、桌面时钟、设备信息三个功能页，以及配网页、离线状态、充电保护确认三个状态页。固件可以完整构建（0 个编译警告，镜像约 4.1 MiB，应用分区 6 MiB）。
- 界面验证只在电脑上做过：模型测试和原生 LVGL 离屏预览（见下面的命令）。**没有在真机上验证过**：屏幕、触摸、电源读数、常亮都还没有实测。
- 界面里已做好但设备上看不到的两处：配网页（后端 R7 还没做，没有代码调用它），充电保护确认框（充电控制 N1 还没做；保护页的开启按钮是禁用的，并写明原因）。
- 没有：管理台前端。`agent/src/deskhub_agent/web/` 不存在时，Agent 只是不挂载 `/manage/`，API 照常工作。
- 固件版本 0.3.0。USB 刷机包和签名 OTA 包已生成在 `dist/`（不在 git 里），哈希和做过的检查记在 [docs/evidence/firmware-0.3.0.json](docs/evidence/firmware-0.3.0.json)。0.3.0 已用 USB 包刷到设备 deskhub-44bd8d60c598：写入校验通过，开机后界面心跳正常、没有错误。但**还没有人看过屏幕**，触摸、真实电量数值和 OTA 回滚都没验证。

运行基线测试：

```sh
.venv/bin/python -m pytest agent/tests -q
.venv/bin/python -m unittest discover -s tools/tests
.venv/bin/python protocol/export_openapi.py --check
cmake -S firmware/tests -B firmware/build-host && cmake --build firmware/build-host && ctest --test-dir firmware/build-host --output-on-failure
python3 firmware/tests/ui_preview/render.py
```

最后一条构建真实的界面代码，在原生 LVGL 里离屏运行 15 组检查，同时运行触摸方向回归测试，并把各页面、各主题的截图存到 `firmware/tests/ui_preview/out/`。结果全部是模拟数据。

完整构建固件（独立目录，不碰别的构建）：

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
unset IDF_COMPONENT_LOCAL_STORAGE_URL
cd firmware && idf.py -B build-0.3.0 build
```

## 不在 git 里的东西

`.state/` 存放签名私钥、管理员凭据、CA 与 TLS 私钥、Agent 数据库和原厂 Flash 备份。它被 `.gitignore` 排除，**不要提交，也不要复制进任何分支**。
`dist/`、固件构建目录、`firmware/managed_components/` 同样被忽略。
