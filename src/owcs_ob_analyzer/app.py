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
from .ui import OverlayWindow


def analyze_image(path: Path) -> dict[str, object]:
    source = ImageFrameSource(path)
    analyzer = RealtimeAnalyzer(max_hero_jobs=2)
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
    return snapshot.as_dict()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="OWCS 游戏内实时观战顶栏分析")
    parser.add_argument("--image", type=Path, help="用一张顶栏截图驱动浮窗，便于校验")
    parser.add_argument("--json", action="store_true", help="分析 --image 并输出 JSON，不启动浮窗")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    if args.json:
        if args.image is None:
            raise SystemExit("--json 必须同时提供 --image")
        print(json.dumps(analyze_image(args.image), ensure_ascii=False, indent=2))
        return
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )
    app = QApplication(sys.argv)
    app.setApplicationName("OWCS OB Analyzer")
    window = OverlayWindow(args.image)
    window.show()
    raise SystemExit(app.exec())


if __name__ == "__main__":
    main()
