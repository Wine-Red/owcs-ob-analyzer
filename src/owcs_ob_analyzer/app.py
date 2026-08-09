from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from .analyzer import RealtimeAnalyzer
from .capture import ImageFrameSource
from .state_output import StateOutputServer
from .ui import OverlayWindow


def analyze_image(path: Path) -> dict[str, object]:
    source = ImageFrameSource(path)
    analyzer = RealtimeAnalyzer(max_hero_jobs=2)
    try:
        snapshot = None
        # Two confirmations for ten slots, processed two at a time.
        for _ in range(22):
            packet = source.grab()
            snapshot = analyzer.analyze(
                packet.image,
                packet.full_size,
                captured_at=packet.captured_at,
                source_name=packet.source_name,
            )
        assert snapshot is not None
        analyzer.settle_player_names()
        packet = source.grab()
        snapshot = analyzer.analyze(
            packet.image,
            packet.full_size,
            captured_at=packet.captured_at,
            source_name=packet.source_name,
        )
        return snapshot.as_dict()
    finally:
        analyzer.close()
        source.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="OWCS 游戏内实时观战顶栏分析")
    parser.add_argument("--image", type=Path, help="用一张顶栏截图驱动浮窗，便于校验")
    parser.add_argument("--json", action="store_true", help="分析 --image 并输出 JSON，不启动浮窗")
    parser.add_argument(
        "--json-output",
        type=Path,
        help="分析 --image 并将 JSON 写入文件，供无控制台 EXE 自检",
    )
    parser.add_argument(
        "--api-host",
        default=os.environ.get("OWCS_OB_API_HOST", "0.0.0.0"),
        help="状态服务监听地址（默认监听所有网卡）",
    )
    parser.add_argument(
        "--api-port",
        type=int,
        default=int(os.environ.get("OWCS_OB_API_PORT", "8765")),
        help="状态服务端口（默认 8765）",
    )
    parser.add_argument(
        "--no-api",
        action="store_true",
        help="不启动 HTTP/WebSocket 状态服务",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()
    if args.json or args.json_output is not None:
        if args.image is None:
            raise SystemExit("--json/--json-output 必须同时提供 --image")
        payload = json.dumps(analyze_image(args.image), ensure_ascii=False, indent=2)
        if args.json_output is not None:
            args.json_output.parent.mkdir(parents=True, exist_ok=True)
            args.json_output.write_text(payload, encoding="utf-8")
        elif sys.stdout is not None:
            print(payload)
        return
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )
    app = QApplication(sys.argv)
    app.setApplicationName("OWCS OB Analyzer")
    state_output = None
    state_output_error = None
    if not args.no_api:
        state_output = StateOutputServer(args.api_host, args.api_port)
        try:
            state_output.start()
        except RuntimeError as exc:
            state_output_error = str(exc)
            state_output = None
    window = OverlayWindow(
        args.image,
        state_output=state_output,
        state_output_error=state_output_error,
    )
    window.show()
    raise SystemExit(app.exec())


if __name__ == "__main__":
    main()
