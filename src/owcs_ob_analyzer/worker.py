from __future__ import annotations

from pathlib import Path
import threading
import time

from PySide6.QtCore import QThread, Signal

from .analyzer import RealtimeAnalyzer
from .capture import FrameSource, ImageFrameSource, create_screen_source


class AnalysisWorker(QThread):
    snapshot_ready = Signal(object)
    failed = Signal(str)
    source_ready = Signal(str)

    def __init__(self, image_path: Path | None = None, target_fps: float = 5.0) -> None:
        super().__init__()
        self.image_path = image_path
        self.target_fps = max(1.0, min(15.0, target_fps))
        self._stop_event = threading.Event()
        self._source: FrameSource | None = None

    def stop(self) -> None:
        self._stop_event.set()

    def run(self) -> None:
        try:
            analyzer = RealtimeAnalyzer()
            self._source = (
                ImageFrameSource(self.image_path)
                if self.image_path is not None
                else create_screen_source(analyzer.layout.top_capture_ratio)
            )
            previous_capture = None
            smoothed_fps = 0.0
            period = 1.0 / self.target_fps
            while not self._stop_event.is_set():
                cycle_started = time.perf_counter()
                packet = self._source.grab()
                if packet is None:
                    self.msleep(8)
                    continue
                if previous_capture is not None:
                    instant = 1.0 / max(1e-3, packet.captured_at - previous_capture)
                    smoothed_fps = instant if smoothed_fps == 0 else smoothed_fps * 0.75 + instant * 0.25
                previous_capture = packet.captured_at
                snapshot = analyzer.analyze(
                    packet.image,
                    packet.full_size,
                    captured_at=packet.captured_at,
                    source_name=packet.source_name,
                    capture_fps=smoothed_fps,
                    desktop_bounds=packet.desktop_bounds,
                )
                self.snapshot_ready.emit(snapshot)
                elapsed = time.perf_counter() - cycle_started
                remaining = period - elapsed
                if remaining > 0:
                    self.msleep(round(remaining * 1000))
        except Exception as exc:
            self.failed.emit(str(exc))
        finally:
            if self._source is not None:
                try:
                    self._source.close()
                except Exception:
                    pass
                self._source = None
