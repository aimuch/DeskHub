#!/usr/bin/env python3
"""Package only ESP-IDF's declared flash artifacts, with offsets and hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.flash_bundle import verify_bundle


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'firmware/build')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    description = json.loads((build / 'project_description.json').read_text())
    version = description['project_version']
    if not isinstance(version, str) or not re.fullmatch(r'[A-Za-z0-9_.+\-]{1,31}', version):
        raise ValueError('Invalid application version in ESP-IDF build metadata')
    if args.output is None:
        args.output = ROOT / f'dist/deskhub-{version}-esp32s3.zip'
    settings = json.loads((build / 'flasher_args.json').read_text())
    if settings.get('extra_esptool_args', {}).get('chip') != 'esp32s3':
        raise ValueError('Expected an ESP32-S3 build')
    if settings.get('flash_settings', {}).get('flash_size') != '16MB':
        raise ValueError('Expected the 16 MB board flash configuration')
    files = {}
    flash_map = {}
    manifest = {'schema_version': 1, 'version': version, 'chip': 'esp32s3', 'board': 'ESP32-S3-Touch-AMOLED-2.16',
                'build_verified': True, 'hardware_verified': False, 'signed_ota': False,
                'flash_settings': settings['flash_settings'], 'files': []}
    for offset, relative in settings['flash_files'].items():
        source = (build / relative).resolve()
        if not source.is_relative_to(build) or source.suffix != '.bin':
            raise ValueError('Unexpected artifact outside the build directory')
        data = source.read_bytes()
        address = int(offset, 16)
        if address < 0 or address + len(data) > 16 * 1024 * 1024:
            raise ValueError('Artifact exceeds the board flash budget')
        files[relative] = data
        flash_map[offset] = relative
        manifest['files'].append({'offset': offset, 'path': relative, 'size_bytes': len(data),
                                  'sha256': hashlib.sha256(data).hexdigest()})
    # Share the standalone installer's restrictions, including image headers,
    # this board's fixed offsets, NVS gaps and complete per-image hashes.
    verify_bundle(build, manifest)
    for relative in ('firmware/dependencies.lock', 'firmware/partitions.csv', 'firmware/sdkconfig.defaults'):
        files[relative] = (ROOT / relative).read_bytes()
    for relative in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'firmware/fonts/OFL.txt', 'firmware/fonts/SourceHanSans-OFL.txt'):
        files[relative] = (ROOT / relative).read_bytes()
    # Preserve component license deliveries alongside the compiled images.
    components = ROOT / 'firmware/managed_components'
    for license_file in sorted(components.rglob('*')):
        if license_file.is_file() and license_file.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
            files['licenses/managed/' + str(license_file.relative_to(components))] = license_file.read_bytes()
    idf_license = Path(description['idf_path']) / 'LICENSE'
    if idf_license.is_file():
        files['licenses/ESP-IDF-LICENSE'] = idf_license.read_bytes()
    files['manifest.json'] = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()
    settings['flash_files'] = flash_map
    files['flasher_args.json'] = (json.dumps(settings, indent=2) + '\n').encode()
    arguments = settings['write_flash_args']
    flash_args = [' '.join(arguments)] + [f'{offset} {relative}' for offset, relative in flash_map.items()]
    files['flash_args'] = ('\n'.join(flash_args) + '\n').encode()
    files['flash.py'] = (ROOT / 'tools/flash_bundle.py').read_bytes()
    files['requirements-flash.txt'] = b'esptool==5.3.1\npyserial==3.5\n'
    files['flashing.md'] = (ROOT / 'docs/flashing.md').read_bytes()
    files['README.txt'] = (
        f'DeskHub {version} USB 刷机包\n'
        '仅适用于 Waveshare ESP32-S3-Touch-AMOLED-2.16 / 16 MiB Flash。\n'
        '完整解压本 ZIP；不需要项目源码、家里的 Agent 或 ESP-IDF。\n\n'
        'Windows（PowerShell，在解压目录）：\n'
        '  py -3 -m venv .flash-venv\n'
        '  .\\.flash-venv\\Scripts\\python.exe -m pip install -r requirements-flash.txt\n'
        '  .\\.flash-venv\\Scripts\\python.exe flash.py --check\n'
        '  .\\.flash-venv\\Scripts\\python.exe flash.py --list\n'
        '  .\\.flash-venv\\Scripts\\python.exe flash.py --port COM5\n\n'
        'macOS / Linux（终端，在解压目录）：\n'
        '  python3 -m venv .flash-venv\n'
        '  .flash-venv/bin/python -m pip install -r requirements-flash.txt\n'
        '  .flash-venv/bin/python flash.py --check\n'
        '  .flash-venv/bin/python flash.py --list\n'
        '  .flash-venv/bin/python flash.py --port /dev/cu.usbmodemXXXX\n\n'
        'COM5 与 /dev/cu.usbmodemXXXX 都只是示例，必须换成 --list 实际显示的端口。\n'
        '脚本先离线校验全部镜像，再只读确认 ESP32-S3 / 16 MiB，最后写入清单中的 4 个镜像。\n'
        '不会全片擦除、写入 NVS/PHY 分区或修改 eFuse；配置会保留。\n'
        '这不是 .dhfw 签名 OTA 包；不要把整个 ZIP 或单个 app 写到 0x0。\n'
        '连接失败时：先关闭串口监视器；按住 BOOT、连接 USB、松开 BOOT。\n'
        '手动进入下载模式后若未启动，完成刷写后重新上电。\n'
        '详情与官方来源见包内 flashing.md。\n\n'
        '功能入口与当前边界：\n'
        '  顶栏状态：点按 Wi-Fi/电脑卡查看连接、RSSI 与 IP，点按电池卡查看电源详情。\n'
        '  电量：顶栏和“设置 → 电池与充电”显示电源芯片电量计的百分比，它是估算值，未对照实际电芯验证；\n'
        '  读不到、数据过期时显示电量未知，无电池和读取故障分别说明，不会显示成 0%。\n'
        '  屏幕常亮：设置 → 显示与亮度 → 屏幕常亮；开启后不进入时钟、不调暗、不息屏，夜间亮度上限与电源保护仍然生效，手动关屏仍然有效。\n'
        '  电池状态：设置 → 电池与充电；80%/90% 充电上限尚未实现，不能设置充电百分比。\n'
        '  设备信息、重启、恢复出厂：设置 → 电池与充电 → 设备信息与重启。\n'
        '  时钟：点击总览时间，或设置 → 桌面时钟。\n'
        '  主题（薄荷夜色/深海蓝/暖橙）、旋转、亮度：设置 → 显示与亮度；向上滑动查看下方选项。\n'
        '  电脑指标、显示器动作：需另行完成当地 Agent 配对、显示器绑定和权限配置。\n'
        '  日常使用通过 Wi-Fi 连接电脑 Agent，无需一直插 USB；无线首次配置向导尚未实现。\n'
        '  网页管理台由电脑 Agent 提供，不是 ESP32 自带网页；USB 刷机不会自动完成配对。\n'
        '  音频、Windows/Linux 控制后端、HomeLab/网络 Provider、任意拖拽排版尚未实现。\n\n'
        'Software build verified; limited hardware results are recorded in repository docs/acceptance.md.\n'
        'Battery, signed OTA, monitor switching and long-duration acceptance remain incomplete.\n'
    ).encode('utf-8')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 27, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    print(json.dumps({'package': str(args.output.resolve()), 'files': len(files),
                      'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest()}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

