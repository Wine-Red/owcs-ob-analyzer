from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from ..models import AnalysisSnapshot
from ..worker import AnalysisWorker
from .hotkeys import GlobalHotkeyPoller
from .player_overlay import PlayerTimelineOverlay
from .timeline_panel import OverallTimelinePanel
from .windowing import bring_to_front


class OverlayWindow(QWidget):
    def __init__(self, image_path: Path | None = None) -> None:
        super().__init__()
        self.image_path = image_path
        self.worker: AnalysisWorker | None = None
        self.player_overlay = PlayerTimelineOverlay()
        self.timeline_panel = OverallTimelinePanel()
        self.overlays_enabled = True
        self.drag_origin: QPoint | None = None
        self._closing = False
        self.setObjectName("overlay")
        self.setWindowTitle("OWCS OB Analyzer")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumWidth(640)
        self.resize(720, 92)
        self._build_ui()
        self._apply_style()
        self.hotkeys = GlobalHotkeyPoller(parent=self, window=self)
        self.hotkeys.visibility_pressed.connect(self.toggle_analysis_overlays)
        self.hotkeys.foreground_pressed.connect(self.bring_overlays_to_front)
        self.hotkeys.status_changed.connect(self._update_hotkey_status)
        self._hotkeys_registered = False

    def _build_ui(self) -> None:
        shell = QFrame(self)
        shell.setObjectName("shell")
        shadow = QGraphicsDropShadowEffect(self)
        shadow.setBlurRadius(28)
        shadow.setOffset(0, 8)
        shadow.setColor(QColor(0, 0, 0, 150))
        shell.setGraphicsEffect(shadow)

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 12, 12, 12)
        root.addWidget(shell)
        body = QVBoxLayout(shell)
        body.setContentsMargins(14, 10, 10, 10)
        body.setSpacing(7)

        header = QHBoxLayout()
        header.setSpacing(8)
        brand = QLabel("OB / LIVE")
        brand.setObjectName("brand")
        header.addWidget(brand)
        self.status_dot = QLabel("●")
        self.status_dot.setObjectName("statusDot")
        header.addWidget(self.status_dot)
        self.status_text = QLabel("未开始")
        self.status_text.setObjectName("statusText")
        header.addWidget(self.status_text)
        self.metrics = QLabel("等待采集画面")
        self.metrics.setObjectName("metrics")
        header.addWidget(self.metrics, 1)
        self.toggle_button = QPushButton("开始识别")
        self.toggle_button.setObjectName("primaryButton")
        self.toggle_button.setAccessibleName("开始或停止实时识别")
        self.toggle_button.clicked.connect(self.toggle_analysis)
        header.addWidget(self.toggle_button)
        self.timeline_button = QPushButton("隐藏浮层")
        self.timeline_button.setAccessibleName("显示或隐藏全部分析浮层")
        self.timeline_button.clicked.connect(self.toggle_analysis_overlays)
        header.addWidget(self.timeline_button)
        self.close_button = QPushButton("×")
        self.close_button.setObjectName("iconButton")
        self.close_button.setToolTip("完全退出")
        self.close_button.setAccessibleName("退出 OWCS OB Analyzer")
        self.close_button.clicked.connect(self.exit_application)
        header.addWidget(self.close_button)
        body.addLayout(header)

        footer = QHBoxLayout()
        self.source = QLabel("采集源 · 主显示器顶部 16%")
        self.source.setObjectName("footer")
        footer.addWidget(self.source)
        footer.addStretch(1)
        self.hotkey_hint = QLabel(f"{GlobalHotkeyPoller.function_summary()} · 拖动移动")
        self.hotkey_hint.setObjectName("footer")
        footer.addWidget(self.hotkey_hint)
        body.addLayout(footer)

    def _apply_style(self) -> None:
        self.setStyleSheet(
            """
            QWidget#overlay { background: transparent; }
            QFrame#shell {
                background: rgba(10, 16, 24, 168);
                border: 1px solid rgba(184, 210, 225, 45);
                border-radius: 14px;
            }
            QLabel { color: #E8F1F5; font-family: "Segoe UI Variable", "Microsoft YaHei UI"; }
            QLabel#brand {
                color: #A6F0DE; font-family: "Bahnschrift SemiCondensed";
                font-size: 15px; font-weight: 700; letter-spacing: 1px;
            }
            QLabel#statusDot { color: #78909C; font-size: 13px; }
            QLabel#statusText { color: #D8E5EA; font-size: 12px; font-weight: 600; }
            QLabel#metrics, QLabel#footer { color: #90A4AE; font-size: 11px; }
            QPushButton {
                min-height: 28px; padding: 0 11px; border-radius: 7px;
                color: #DDE9ED; background: rgba(255, 255, 255, 12);
                border: 1px solid rgba(255, 255, 255, 22);
                font-family: "Segoe UI Variable", "Microsoft YaHei UI"; font-size: 11px;
            }
            QPushButton:hover { background: rgba(255, 255, 255, 24); }
            QPushButton:focus { border-color: #8CE8D0; }
            QPushButton#primaryButton {
                color: #08211C; background: #8CE8D0; border-color: #A9F5E1; font-weight: 700;
            }
            QPushButton#primaryButton:hover { background: #A7F2DF; }
            QPushButton#iconButton { min-width: 30px; max-width: 30px; padding: 0; font-size: 16px; }
            """
        )

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._hotkeys_registered:
            self.hotkeys.register()
            self._hotkeys_registered = True
        if not self.property("positioned"):
            screen = QApplication.primaryScreen().availableGeometry()
            self.move(screen.center().x() - self.width() // 2, screen.bottom() - self.height() - 24)
            self.setProperty("positioned", True)
        if self.image_path is not None and self.worker is None:
            QTimer.singleShot(0, self.start_analysis)
        QTimer.singleShot(0, self.adjustSize)

    def toggle_analysis(self) -> None:
        if self.worker is not None and self.worker.isRunning():
            self.stop_analysis()
        else:
            self.start_analysis()

    def start_analysis(self) -> None:
        if self.worker is not None and self.worker.isRunning():
            return
        self.status_dot.setStyleSheet("color: #FFD27A")
        self.status_text.setText("正在初始化")
        self.toggle_button.setText("停止识别")
        worker = AnalysisWorker(self.image_path)
        worker.snapshot_ready.connect(self.update_snapshot)
        worker.failed.connect(self.handle_error)
        worker.finished.connect(self.worker_finished)
        self.worker = worker
        worker.start()

    def stop_analysis(self) -> None:
        if self.worker is None:
            return
        self.status_text.setText("正在停止")
        self.worker.stop()
        self.toggle_button.setEnabled(False)

    def worker_finished(self) -> None:
        self.worker = None
        self.toggle_button.setEnabled(True)
        self.toggle_button.setText("开始识别")
        if self.status_text.text() == "正在停止":
            self.status_dot.setStyleSheet("color: #78909C")
            self.status_text.setText("已停止")
        self.player_overlay.hide()
        self.timeline_panel.hide()

    def update_snapshot(self, snapshot: AnalysisSnapshot) -> None:
        self.status_dot.setStyleSheet("color: #72DDB5")
        self.status_text.setText("实时识别中")
        fps = f"{snapshot.capture_fps:.1f} FPS" if snapshot.capture_fps else "采样中"
        self.metrics.setText(
            f"{fps}  ·  本轮 {snapshot.cycle_latency_ms:.0f} ms  ·  英雄复核 {snapshot.classified_slots}/2"
        )
        self.source.setText(f"采集源 · {snapshot.source_name}")
        self.player_overlay.update_snapshot(snapshot)
        self.timeline_panel.update_snapshot(snapshot)

    def handle_error(self, message: str) -> None:
        self.status_dot.setStyleSheet("color: #FF756F")
        self.status_text.setText("采集失败")
        self.metrics.setText(message)
        self.metrics.setToolTip(message)
        self.player_overlay.hide()
        self.timeline_panel.hide()

    def toggle_analysis_overlays(self) -> None:
        enabled = not self.overlays_enabled
        self.overlays_enabled = enabled
        self.player_overlay.set_enabled(enabled)
        self.timeline_panel.set_enabled(enabled)
        self.timeline_button.setText("隐藏浮层" if enabled else "显示浮层")

    def bring_overlays_to_front(self) -> None:
        if not self.overlays_enabled:
            self.overlays_enabled = True
            self.player_overlay.set_enabled(True)
            self.timeline_panel.set_enabled(True)
            self.timeline_button.setText("隐藏浮层")
        self.raise_()
        self.player_overlay.raise_()
        self.timeline_panel.raise_()
        bring_to_front(self)

    def _update_hotkey_status(self, status: str) -> None:
        self.hotkey_hint.setText(f"{status} · 拖动移动")

    def exit_application(self) -> None:
        self.close()

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.drag_origin = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()

    def mouseMoveEvent(self, event) -> None:
        if self.drag_origin is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self.drag_origin)
            event.accept()

    def mouseReleaseEvent(self, event) -> None:
        self.drag_origin = None
        super().mouseReleaseEvent(event)

    def closeEvent(self, event) -> None:
        if self._closing:
            event.accept()
            return
        self._closing = True
        self.hotkeys.shutdown()
        self._hotkeys_registered = False
        worker = self.worker
        if worker is not None and worker.isRunning():
            worker.stop()
            if not worker.wait(5000):
                worker.terminate()
                worker.wait(1000)
        self.worker = None
        self.player_overlay.close()
        self.timeline_panel.close()
        super().closeEvent(event)
        app = QApplication.instance()
        if app is not None:
            QTimer.singleShot(0, app.quit)
