from __future__ import annotations

import asyncio
from copy import deepcopy
import time

import psutil

from .models import stamp


class HostMetrics:
    def __init__(self, host_id: str):
        self.host_id = host_id
        self.previous_network = None
        self.previous_tick = None
        self.updated_tick = None
        self.snapshot = {"host_id": host_id, "online": True, "metrics": []}
        self.cpu_primed = False

    def collect(self) -> dict:
        now, tick = stamp(), time.monotonic()
        metrics = []

        def add(key, label, value, unit, quality="valid", reason=None):
            metrics.append({"key": key, "label": label, "value": value, "unit": unit, "source": "host-agent:psutil",
                            "observed_at": now, "ttl_ms": 6000, "quality": quality, "reason_code": reason})

        try:
            cpu = psutil.cpu_percent(interval=None)
            add("cpu.utilization", "CPU", cpu if self.cpu_primed else None, "percent",
                "valid" if self.cpu_primed else "unavailable", None if self.cpu_primed else "SAMPLER_WARMUP")
            self.cpu_primed = True
        except (psutil.Error, OSError):
            add("cpu.utilization", "CPU", None, "percent", "unavailable", "SENSOR_UNAVAILABLE")
        try:
            memory = psutil.virtual_memory()
            add("memory.utilization", "内存", memory.percent, "percent")
            add("memory.used", "已用内存", memory.used, "byte")
            add("memory.total", "总内存", memory.total, "byte")
        except (psutil.Error, OSError):
            add("memory.utilization", "内存", None, "percent", "unavailable", "SENSOR_UNAVAILABLE")
        try:
            network = psutil.net_io_counters()
            elapsed = tick - self.previous_tick if self.previous_tick else 0
            for key, label, attr in (("network.receive_rate", "网络接收", "bytes_recv"), ("network.send_rate", "网络发送", "bytes_sent")):
                delta = getattr(network, attr) - getattr(self.previous_network, attr) if self.previous_network else -1
                valid = elapsed > 0 and delta >= 0
                add(key, label, delta / elapsed if valid else None, "byte_per_second", "valid" if valid else "unavailable",
                    None if valid else "SAMPLER_WARMUP_OR_COUNTER_RESET")
            self.previous_network, self.previous_tick = network, tick
        except (psutil.Error, OSError):
            add("network.receive_rate", "网络接收", None, "byte_per_second", "unavailable", "SENSOR_UNAVAILABLE")
            add("network.send_rate", "网络发送", None, "byte_per_second", "unavailable", "SENSOR_UNAVAILABLE")
        try:
            add("system.uptime", "运行时间", max(0, time.time() - psutil.boot_time()), "duration_seconds")
        except (psutil.Error, OSError):
            add("system.uptime", "运行时间", None, "duration_seconds", "unavailable", "SENSOR_UNAVAILABLE")
        # psutil does not provide a portable, unambiguous CPU die sensor. Do not
        # relabel board/GPU/disk sensors as CPU temperature.
        add("cpu.temperature", "CPU 温度", None, "celsius", "unsupported", "CPU_TEMPERATURE_NOT_IMPLEMENTED")
        self.updated_tick = tick
        self.snapshot = {"host_id": self.host_id, "online": True, "observed_at": now, "metrics": metrics}
        return self.snapshot

    def read(self) -> dict:
        snapshot = deepcopy(self.snapshot)
        if self.updated_tick is None or time.monotonic() - self.updated_tick > 6:
            for metric in snapshot["metrics"]:
                if metric["quality"] == "valid":
                    metric["quality"] = "stale"
        return snapshot

    async def run(self) -> None:
        while True:
            self.collect()
            await asyncio.sleep(2)
