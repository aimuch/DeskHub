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
git diff --stat archive/v0.2 main -- tools protocol firmware/components/core firmware/components/management firmware/components/agent_client firmware/main firmware/tests ':!firmware/tests/board_cache_test.cpp'
```

输出为空，就表示这些文件与归档逐字节相同。

已按规则 K7 单独提交改动的基线文件（所以上面的命令不含它们）：

| 文件 | 改动 |
| --- | --- |
| `firmware/components/board_support/`、`firmware/tests/board_cache_test.cpp` | 读取电池电压、USB 电压、芯片温度，并加入电源芯片写入白名单（规则 R3），附 18 条失败模式的测试 |

## 文档

- [产品范围](docs/product-scope.md)：做什么、不做什么、每个功能的状态，以及 14 条决策记录。
- [规则](docs/rules.md)：保留的安全规则，以及放宽的规则和用来代替的保护。
- [设计稿](docs/design/README.md)：设计稿源文件、画板清单和设计变量。

## 现状

- 有：Agent 的 API（没有管理台）、固件的核心、板级、管理、Agent 客户端和 USB 协议、签名与配对工具、测试。
- 没有：设备界面（`firmware/components/ui`）。新界面就位前，固件不能完整构建。
- 没有：管理台前端。`agent/src/deskhub_agent/web/` 不存在时，Agent 只是不挂载 `/manage/`，API 照常工作。

运行基线测试：

```sh
.venv/bin/python -m pytest agent/tests -q
.venv/bin/python -m unittest discover -s tools/tests
.venv/bin/python protocol/export_openapi.py --check
cmake -S firmware/tests -B firmware/build-host && cmake --build firmware/build-host && ctest --test-dir firmware/build-host --output-on-failure
```

## 不在 git 里的东西

`.state/` 存放签名私钥、管理员凭据、CA 与 TLS 私钥、Agent 数据库和原厂 Flash 备份。它被 `.gitignore` 排除，**不要提交，也不要复制进任何分支**。
`dist/`、固件构建目录、`firmware/managed_components/` 同样被忽略。
