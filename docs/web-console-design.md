# 网页管理台（C7）

状态：**已实现**，Agent 在 `/manage/` 提供。没有构建步骤：`agent/src/deskhub_agent/web/` 下是 HTML、CSS 和原生 JavaScript 模块。
设计来自设计稿的 `Web-*` 画板（[设计稿说明](design/README.md)）。登录和密码的规则见 [管理员登录的设计](admin-login-design.md)。

## 页面和它们用的接口

| 页面 | 接口 | 和设计稿不同的地方 |
| --- | --- | --- |
| 概览 | `overview`、`host`、`devices`、`commands` | 状态环四段是“设备连接、电脑、电源、配置”，第四段不是“时间”：设备不上报时间状态，网页不编。色卡里没有数据的写“未上报” |
| 设备 | `devices`、`devices/{id}`、`firmware` | 没有“重启设备”“恢复出厂”：没有这样的接口，不放不会生效的按钮。配置状态按步骤列出；“升级信任”显示设备和固件包的公钥指纹，Agent 只显示“已配置”（接口不给指纹） |
| 动作与显示器 | `displays` | 只读，绑定标识不显示 |
| 命令记录 | `commands` | 加了筛选（全部、已确认、未确认、失败）和右边的细节 |
| 布局与显示 | `devices/{id}/configuration` | 预览是 HTML 示意，不是设备的真实界面（浏览器里画不了 LVGL）。页面上这么写明 |
| 电源 | `devices/{id}` 的 `power` | 没有“测量状态”的寄存器明细，也没有充电保护开关和充电事件：设备上报里没有这些，网页不放假开关。过期的读数写“未上报” |
| 固件升级 | `firmware`、`devices/{id}/updates` | 页面要求 `.dhfw`；你决定不做签名升级包，所以这页现在主要是空的固件库 |
| 访问管理 | `devices`、`devices/{id}/revoke`、`pairing-grants`、`password` | 加了“无线配对”（生成一次性配对包、授权记录、取消）；没有“吊销口令”：网页看不到口令，用命令行吊销 |
| 外观 | 不用接口 | 只有简体中文；字体是系统字体的三种选择（外部字体被 CSP 挡住）；设备的外观只读说明；管理台外观可导出、导入、恢复默认 |
| 登录、设置密码 | `auth`、`login`、`session`、`password` | “信任证书”一栏只写步骤，不提供下载：从还不被信任的页面下载信任根，等于没有验证 |

## 安全约定

- CSP 是 `script-src 'self'; style-src 'self'`：页面没有内联脚本和样式，没有外部字体和脚本。
- 不用 `innerHTML`；所有文本经文本节点写入。
- 浏览器里只存一个键 `deskhub.appearance`（外观选择）；没有任何登录信息。登录状态只在 HttpOnly Cookie 里；CSRF 令牌只在内存里。
- 每个写请求带 `X-CSRF-Token` 和同源 Origin；两个登录入口是例外（那时还没有会话）。

## 失败模式和测试

| 编号 | 失败模式 | 在哪里测 |
| --- | --- | --- |
| W1 到 W8 | 内联脚本或样式、外部资源、不能解析的脚本、`innerHTML`、存储里有机密、导航和设计不一致、未登录就有数据、写请求没有 CSRF、“实验功能”字样 | `agent/tests/test_web_console.py` |
| E | 完整流程：第一次口令登录只能设置密码、设置密码、九个页面、命令记录、布局保存到设备确认、电源、固件包被拒绝、无线配对授权、吊销、修改密码（其他会话立即失效）、外观（导出、导入、恢复）、退出、五次输错锁定、命令行重置结束已打开的会话、窄屏没有横向滚动、数据过期、全程没有脚本异常和 CSP 违规 | `scripts/e2e/console.e2e.mjs`：真实的 Agent 进程加真实的 Chrome（CDP，真实键入和点击）。证据：`docs/evidence/console-e2e/`（`report.json` 加截图） |
| M | 故意改坏前端，E2E 要报红 | `scripts/mutation/console_ui.py` |

替身有两处，写在 `report.json` 里：设备上报由脚本模拟（调用真实的 `/api/v1/device/sync`），显示器后端是 `scripts/e2e/fake_betterdisplay.py`。

## 没有验证的

- 没有接真实设备，没有接真实显示器。
- 只用了 Chrome。Safari 和 Firefox 没有试过。
- 测试用本机 HTTP；HTTPS 下 Cookie 的 `Secure` 标志在 pytest 里测。
- 只有简体中文。
