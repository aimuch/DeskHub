# 在另一台电脑上通过 USB 刷机

把 `deskhub-<版本>-esp32s3.zip` **完整复制并解压**到单位电脑。包内的 `flash.py` 可独立使用，不需要源码、ESP-IDF 或家里的 Agent。电脑需要 Python 3.10 以上；建议使用 Python 3.12/3.13，并在包目录创建专用虚拟环境。

这是 Waveshare **ESP32-S3-Touch-AMOLED-2.16、16 MiB Flash** 的 USB 包。`.dhfw` 是管理台使用的签名应用更新包，不能替代这里的 USB 包。不要把整个 ZIP，或单独的 `deskhub.bin`，按官方出厂单镜像的偏移写入。

## Windows

安装 Python 后，在解压目录打开 PowerShell。直接调用虚拟环境的 Python，不需要修改 PowerShell 执行策略：

```powershell
py -3 -m venv .flash-venv
.\.flash-venv\Scripts\python.exe -m pip install -r requirements-flash.txt
.\.flash-venv\Scripts\python.exe flash.py --check
.\.flash-venv\Scripts\python.exe flash.py --list
.\.flash-venv\Scripts\python.exe flash.py --port COM5
```

把示例 `COM5` 换成 `--list` 中实际出现的设备端口。没有 `py` 命令时，可用已安装的 `python` 执行第一条命令；后续保持虚拟环境中的完整路径。

## macOS / Linux

在解压目录运行：

```sh
python3 -m venv .flash-venv
.flash-venv/bin/python -m pip install -r requirements-flash.txt
.flash-venv/bin/python flash.py --check
.flash-venv/bin/python flash.py --list
.flash-venv/bin/python flash.py --port /dev/cu.usbmodemXXXX
```

macOS 通常是 `/dev/cu.usbmodem…`，Linux 通常是 `/dev/ttyACM…`。它们只是路径示例，实际以列表为准。Linux 若提示串口权限不足，请按单位系统管理规范授予串口访问权限，不要把整个刷机流程改成管理员或 root 运行。

初次安装依赖需要能访问 Python 包索引。单位限制联网或安装软件时，应先按单位 IT 规则准备 Python 与依赖，不能仅复制家里 Mac 的 `.venv` 给 Windows 使用。依赖固定为官方 `esptool==5.3.1`、`pyserial==3.5`；[esptool 安装文档](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)说明了独立安装方式。

## 工具会检查什么

`--check` 只做离线检查，不打开串口：验证清单、目标板/芯片、4 个镜像的大小与 SHA-256、ESP32-S3 镜像头、应用项目名与版本，以及固定分区偏移。路径越界、重复、重叠、损坏或不完整的包会在写入前拒绝。

`--list` 只列串口；工具不会自动选择第一台设备。指定 `--port` 后，还会先用只读 `flash-id` 确认 **ESP32-S3 与 16 MiB Flash**，再通过 ROM loader（`--no-stub`）写入以下镜像：

| 偏移 | 文件 |
| --- | --- |
| `0x0` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0xf000` | `ota_data_initial.bin` |
| `0x20000` | `deskhub.bin` |

esptool 会显示写入进度并校验镜像；只有它正常结束，脚本才报告刷写完成。写入失败或中断不能视为成功，可以重新进入下载模式重刷同一完整包。

本包不执行全片擦除，不写 NVS/PHY 数据区或 eFuse。已有 Wi-Fi、配对、主题、屏幕方向等配置保留；USB 安装会重新建立此布局的启动信息并从 `ota_0` 启动。后续联网仍需要设备能访问实际 Agent：搬到单位不等于能访问家里的 `10.x` 私网地址，需在单位另行安装 Agent 与工程 USB 配置工具，按工程的 `docs/provisioning.md` 配置当地的 Wi-Fi、Agent 地址与信任。

刷机包不包含 Wi-Fi 密码、设备 Token、签名私钥或原厂整片备份。不要通过公开文件传输配置凭据。

## 端口不出现或连接失败

先使用可传输数据的 USB 线，关闭占用同一端口的串口 monitor 或其他刷机工具。正常运行的 DeskHub 原生 USB 通常可以由 esptool 自动进入下载模式，无需每次按 BOOT。

若无法识别，按微雪本板说明：**按住 BOOT → 连接 USB → 松开 BOOT**，重新列出端口后再刷。固件崩溃时按官方 FAQ 先完全断电，再按住 BOOT 上电进入下载模式。手动进入下载模式后可能不会自动退出，刷写完成后重新上电运行。

官方来源：[微雪使用说明](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Instructions-For-Use)、[本板 FAQ](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/FAQ)。官方出厂单镜像/擦除示例不等于本项目的多镜像布局；本工具只使用上表和包清单，不执行示例中的全片擦除步骤。

软件检查与 USB 写入成功不代表屏幕方向、触摸、电源、OTA 回滚或长时间运行已验收。设备已带到单位，本次新版本的真机观察须在单位记录，不能用离线检查替代。
