#!/usr/bin/env python3
"""E2E 测试替身：模拟 BetterDisplay CLI 的外部可执行文件，仅用于 scripts/e2e。

Agent 与真实后端一样把它当子进程调用（固定 argv、无 shell）。它不是生产后端，也不代表任何
显示器行为。状态保存在 $FAKE_BD_STATE 目录；把下列名字（空白分隔）写入该目录的 faults 文件即可注入
故障，写空内容即清除：
  fail_inventory  get --identifiers 失败（命令在下发前失败）
  fail_set        set 失败（已下发后失败 → 未确认）
  fail_get        读回失败（读不到输入源 → 未确认）
"""
import json
import os
import sys
from pathlib import Path

state = Path(os.environ["FAKE_BD_STATE"])
state.mkdir(parents=True, exist_ok=True)
args = sys.argv[1:]


def flag(name: str) -> bool:
    faults = state / "faults"
    return faults.exists() and name in faults.read_text().split()


if args[:1] == ["help"]:
    print("BetterDisplay Version 0.0-e2e-fixture\nTest fixture, not the real application.")
elif args[:2] == ["get", "--identifiers"]:
    if flag("fail_inventory"):
        sys.exit(1)
    print(json.dumps([{"UUID": "E2E-UUID-0001", "serial": "E2E123", "registryLocation": "IOService:/e2e/monitor", "name": "E2E Display"}]))
elif args[:1] == ["set"]:
    if flag("fail_set"):
        sys.exit(1)
    value = [a.split("=", 1)[1] for a in args if a.startswith("--value=")][0]
    (state / "input").write_text(value)
elif args[:1] == ["get"] and "--value" in args:
    if flag("fail_get"):
        sys.exit(1)
    print((state / "input").read_text() if (state / "input").exists() else "15")
else:
    sys.exit(2)
