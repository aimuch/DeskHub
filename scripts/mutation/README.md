# 变异检查

目的：故意改坏实现，看测试会不会报红。
测试没有报红，说明少了一条测试。

## 用法

```bash
scripts/mutation/pairing_agent.py      # Agent 端配对：改 agent/src/deskhub_agent/pairing.py，跑 test_pairing_grants.py
scripts/mutation/pairing_firmware.py   # 设备端配对：改 firmware/components/pairing/，跑主机上的 pairing_test
scripts/mutation/charge_firmware.py    # 充电保护的策略：改 firmware/components/charge/，跑 charge_test
scripts/mutation/charge_ui.py          # 充电保护的界面：改 ui_model.cpp、ui_pages.cpp、ui.cpp，跑 ui_model_test 和界面预览
scripts/mutation/header_ui.py           # 图标顶栏、电池百分比和圆环粗细：改 ui_model.cpp、ui_pages.cpp，跑 ui_model_test 和界面预览
scripts/mutation/corners_ui.py          # 屏幕圆角：改 ui_pages.cpp 里避开圆角的位置，跑界面预览里的圆角几何测试
scripts/mutation/pairing_glue.py        # 设备端配对接线里能在主机上测的部分：改 pairing_glue.cpp，跑 pairing_glue_test
scripts/mutation/admin_password.py      # 管理员密码：改 admin_auth.py、management.py、cli.py，跑 test_admin_password.py
scripts/mutation/console_ui.py          # 网页管理台：改前端脚本，跑 scripts/e2e/console.e2e.mjs（真实的 Agent 加 Chrome，每处约 2 分钟）
```

这些脚本都可以带名字片段，只跑名字里含这些片段的变异：

```bash
scripts/mutation/pairing_firmware.py "CA" "PEM"
```

## 运行条件

- Agent 端：仓库根目录的 `.venv` 里要有 pytest。没有 `.venv` 时，脚本用当前的 Python。
- 设备端：本机要有 `cmake` 和 C++ 编译器。不需要 ESP-IDF，也不需要设备。
- `charge_ui.py` 的界面预览部分另外要 `ninja`，以及 `firmware/managed_components` 里已经取回的 LVGL（先做过一次 `idf.py reconfigure`）。第一次要编译 LVGL，比较慢。

## 工作方式

- 脚本把代码复制到临时目录，只改副本。工作区的文件不会被改。
- 先跑一遍没有改坏的代码。这一遍必须全绿，否则脚本直接退出。
- 然后一次只改坏一处，跑测试，再改回来。
- 每一条变异都会删掉目标的目标文件再重新编译。原因：`make` 只比较到秒的文件时间，源码在同一秒内改回再改坏，它会以为没变，运行上一条变异的程序。
- 只看测试程序的退出码，输出丢掉。否则一条让每一步都失败的变异会把脚本的内存撑满。

## 输出的含义

| 输出 | 意思 | 怎么办 |
|---|---|---|
| `caught` | 测试报红了 | 这是对的 |
| `SURVIVED` | 改坏了，测试还是绿的 | 缺一条测试。先补测试，再确认它报红 |
| `SKIP` | 表里的旧片段在文件里找不到 | 实现改过了。更新表里的片段 |
| `INVALID` | 改坏后的设备端代码编译不过（`-Werror`） | 换一种改法，让它能编译 |

只有基线全绿、每一处都是 `caught`，退出码才是 0。

## 增加一处变异

在脚本开头的表 `M` 里加一行。
一行是：名字、文件、旧片段、新片段。
旧片段在文件里只替换第一处。所以它要足够长，才能指到你想改的那一处。

新的变异先要有失败模式：这一处改坏后，用户会看到什么错？
写不出来，就不要加。
