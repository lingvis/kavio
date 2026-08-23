#!/usr/bin/env python3
"""Kavio Python 指标矩形 Demo。

设备端每两秒向 Host Component 查询一次 Windows CPU、内存和磁盘数据，绘制成
一个 560x220 BGRA8888 TX 矩形。Host Component 提供的 App Panel 拖动矩形后，会把新坐标
发送回来；下一帧立即使用新的 dest_pos_x / dest_pos_y。
"""

from __future__ import annotations

import signal
import threading
import time
from typing import Optional

import protocol
import renderer
from kavio_bridge import KavioBridge

QUERY_INTERVAL_SECONDS = 2.0
READY_INTERVAL_SECONDS = 1.0

# SDK 回调可能来自后台线程，因此主循环只读取受锁保护的轻量状态快照；
# 真正的渲染和 TX 写入始终在 main() 所在线程完成。
_stop = threading.Event()
_lock = threading.Lock()
_client_ready = False
_channel_ready = False
_position = (40, 40)
_metrics: Optional[protocol.Metrics] = None
_generation = 0


def _set_client_ready(ready: bool) -> None:
    global _client_ready
    with _lock:
        _client_ready = ready


def _set_channel_ready(ready: bool) -> None:
    global _channel_ready
    with _lock:
        _channel_ready = ready


# 一个 Channel 回调对应一条完整业务数据报。先尝试按 Response 解析，再按 Position 解析。
def _on_channel_data(payload: bytes) -> None:
    global _metrics, _position, _generation
    metrics = protocol.parse_response(payload)
    position = protocol.parse_position(payload)
    with _lock:
        if metrics is not None:
            _metrics = metrics
            _generation += 1
        elif position is not None:
            _position = (max(0, position[0]), max(0, position[1]))
            _generation += 1


def _bytes_text(value: int) -> str:
    return f"{value / (1024 ** 3):.1f} GB"


# renderer 只认识字符串列表；这里负责把业务对象转换成最终展示文本。
def _render_lines(metrics: Optional[protocol.Metrics], position: tuple[int, int]) -> list[str]:
    lines = ["PYTHON METRICS DEMO", f"POS: {position[0]} / {position[1]}"]
    if metrics is None:
        lines.append("WAITING FOR HOST")
        return lines
    lines.append(f"CPU: {metrics.cpu_usage_percent:.1f}%")
    lines.append(
        "MEM: " + _bytes_text(metrics.memory_used_bytes) +
        " / " + _bytes_text(metrics.memory_total_bytes)
    )
    for disk in metrics.disks[:3]:
        lines.append(
            f"{disk.name}: {_bytes_text(disk.used_bytes)} / {_bytes_text(disk.total_bytes)}"
        )
    return lines


def _signal_handler(_signum: int, _frame: object) -> None:
    _stop.set()


def main() -> int:
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    bridge = KavioBridge(
        client_name="demo_metrics_panel",
        on_ready=_set_client_ready,
        on_channel=_set_channel_ready,
        on_data=_on_channel_data,
    )

    sequence = 1
    frame_id = 1
    rendered_generation = -1
    next_query = 0.0
    next_ready = 0.0

    try:
        while not _stop.is_set():
            now = time.monotonic()
            with _lock:
                client_ready = _client_ready
                channel_ready = _channel_ready
                generation = _generation
                position = _position
                metrics = _metrics

            # Client ready 之后才能创建 TX Surface；bridge 会在 session 重建后自动丢弃旧 surface。
            if client_ready:
                bridge.ensure_surface(renderer.WIDTH, renderer.HEIGHT)

            # DeviceReady 是业务层就绪声明，Host 收到后才会发送 Position。
            if channel_ready and now >= next_ready:
                bridge.send_channel_data(protocol.build_device_ready())
                next_ready = now + READY_INTERVAL_SECONDS

            if channel_ready and now >= next_query:
                bridge.send_channel_data(protocol.build_query(sequence))
                sequence += 1
                next_query = now + QUERY_INTERVAL_SECONDS

            # 只有指标或位置发生变化时才重新生成像素，避免 30ms 主循环持续重画静态内容。
            if client_ready and generation != rendered_generation:
                pixels = renderer.render(_render_lines(metrics, position))
                if bridge.write_frame(
                    pixels,
                    renderer.WIDTH,
                    renderer.HEIGHT,
                    renderer.STRIDE,
                    position[0],
                    position[1],
                    frame_id,
                ):
                    frame_id += 1
                    rendered_generation = generation

            time.sleep(0.03)
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
