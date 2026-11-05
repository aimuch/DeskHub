# 屏幕软键盘换 Wi-Fi：主机验证证据

日期：2026-11-03。**仅主机模拟，没有连接设备，没有真机联网证据。**

- `ui-report.json`：原生 LVGL 的全部 26 组流程通过；新组 `wifi_keyboard` 使用真实的 LVGL 指针事件，Wi-Fi 服务由测试替身提供。
- `wifi-keyboard.png`、`wifi-asking.png`、`wifi-saved.png`：真实 UI 源码的离屏渲染，均为示意数据。
- `keyboard-mutation.txt`：键盘模型 7 处变异全部发现；基线先通过，再逐项改坏实现。
- `keyboard-ui-mutation.txt`：触屏流程 4 处变异全部发现，包括短密码、松手不隐藏、拖出仍触发和重建后持续删除。
- `wifi-change-mutation.txt`：换网状态机 8 处变异全部发现，包括无线占用、密码校验、扫描去重、失败恢复、60 秒默认回退和保存失败。
- `verification.json`：构建、主机检查、工具回归和产物哈希。构建目录在临时目录；没有覆盖 `dist/` 中的发布包。

## 重复验证

在仓库根目录执行。下面的 `dh-wifi-*` 是临时构建目录，不使用真实配置。

```bash
cmake -S firmware/tests -B /tmp/dh-wifi-host -G Ninja
```

```bash
cmake --build /tmp/dh-wifi-host
```

```bash
ctest --test-dir /tmp/dh-wifi-host --output-on-failure
```

```bash
.venv/bin/python -m unittest discover -s tools/tests -q
```

```bash
cmake -S firmware/tests/ui_preview -B /tmp/dh-wifi-preview -G Ninja
```

```bash
cmake --build /tmp/dh-wifi-preview
```

只验证新触屏流程：

```bash
DESKHUB_PREVIEW_CASE=wifi_keyboard /tmp/dh-wifi-preview/deskhub_ui_preview /tmp/dh-wifi-preview-out
```

开发结束后跑完整界面流程：

```bash
/tmp/dh-wifi-preview/deskhub_ui_preview /tmp/dh-wifi-preview-out
```

截图先输出为 PPM，可用 macOS 的 `sips -s format png` 转换。

```bash
python3 scripts/mutation/keyboard.py
```

```bash
python3 scripts/mutation/wifi_keyboard_ui.py
```

```bash
python3 scripts/mutation/wifi_change.py
```

ESP32 交叉构建（独立目录，不刷机）：

```bash
/bin/bash -c 'source ~/.espressif/tools/activate_idf_v6.0.2.sh && idf.py -C firmware -B /tmp/dh-wifi-idf build'
```

## 验证边界

主机测试证明输入规则、状态机和界面交互，不证明 ESP32 无线驱动、NVS 断电原子性、真实路由器、Agent 网络可达性或触屏手感。需要在设备旁验证扫描、密码错误回退、连上 Wi-Fi 但找不到电脑后的 60 秒回退、保存后重启，以及实际按键大小。

这份证据写于主机验证时，当时固件还是临时的 0.3.8 构建。之后升版并刷到设备，最新是 0.3.13（见 `../firmware-0.3.13.json`）。代码已在 2026-11-05 按 K4 和 K7 分组提交。真机上的换网流程仍然没有走完。
