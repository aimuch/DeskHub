# DeskHub 桌面 App

菜单栏里的小程序：点一下就把显示器切到另一台电脑。它用 DDC 直接切，不需要 BetterDisplay，也不需要 Agent。
以后桌上的触摸屏会通过蓝牙连它（还没做，见 [设计文档](../docs/desktop-app-design.md) 第 13 节）。

只支持 Apple 芯片的 Mac，macOS 13 以上。

## 构建和安装

不需要 Xcode，命令行工具就够：

```bash
desktop/scripts/make_app.sh
```

做好的 App 在 `desktop/build/DeskHub.app`。拖到“应用程序”里就行。第一次打开时，系统可能要你在“系统设置 → 隐私与安全性”里允许它（没有公证）。

## 用法

1. 点菜单栏的显示器图标 → 打开设置 → **显示器**：绑定你的显示器。绑定时读到的输入，就是这台 Mac 自己（切回来的路）。
2. **输入源**：改名字、选图标，然后**试切**。试切会切过去，20 秒后自动切回这台 Mac。只有显示器确实切过去、又确认切回来的输入源，才会出现在菜单里。
3. 菜单里点输入源就切过去。结果有三种：**已确认**（显示器读回的就是它）、**已发送（未确认）**（读不回或读回不一样）、**没有发出**（写不进去，写明原因）。切走以后，这台 Mac 的画面就没了，用显示器按键或以后的触摸屏切回来。

配置在 `~/Library/Application Support/DeskHub/config.json`，每次切换的结果记在 `~/Library/Logs/DeskHub/switch.jsonl`。

## 开发

| 目标 | 内容 |
| --- | --- |
| `DeskHubCore` | 规则：绑定、白名单、结果怎么判断、配置（K2、K3）。没有 AppKit 和 IOKit |
| `DDC` | 通过 IOAVService（系统私有接口）读写显示器。App 里只有它和显示器说话 |
| `DeskHub` | 菜单栏 App（SwiftUI） |
| `deskhub-ddc` | 命令行：`list`、`get <uuid> 60`、`switch …`（会切走屏幕，要有人看着） |
| `core-check` | 规则的检查：`swift run core-check`，失败返回 1 |

这台 Mac 只有命令行工具时要注意：`swift test` 不能用（没有 Testing 的宏插件），所以检查是一个普通程序；SwiftUI 的 `@State` 也不能用（新 SDK 里它是宏），用 `@StateObject` 加小的 `ObservableObject` 代替。

DDC 的做法参考了 m1ddc 和 AppleSiliconDDC（都是 MIT，作者 waydabber），代码是按协议自己写的，没有复制。
