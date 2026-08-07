from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPoint, QSize, Qt, QTimer
from PySide6.QtGui import QColor, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from ..models import AnalysisSnapshot, SlotSnapshot
from ..worker import AnalysisWorker
from .hotkeys import GlobalHotkeyPoller
from .player_overlay import PlayerTimelineOverlay, load_hero_icon
from .timeline_panel import OverallTimelinePanel


HERO_LABELS = {
    "soldier-76": "Soldier: 76",
    "wrecking-ball": "Wrecking Ball",
    "junker-queen": "Junker Queen",
    "jetpack-cat": "Jetpack Cat",
}


class PlayerCard(QFrame):
    def __init__(self, side: str, slot: int) -> None:
        super().__init__()
        self.side = side
        self.slot = slot
        self.setObjectName("playerCard")
        self.setProperty("life", "unknown")
        self.setMinimumWidth(108)
        self.setAccessibleName(f"{side} {slot + 1}号选手")

        row = QHBoxLayout(self)
        row.setContentsMargins(7, 6, 7, 6)
        row.setSpacing(7)
        self.icon = QLabel("?")
        self.icon.setObjectName("heroIcon")
        self.icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.icon.setFixedSize(34, 34)
        row.addWidget(self.icon)

        copy = QVBoxLayout()
        copy.setSpacing(0)
        self.player_name = QLabel(f"{'左' if side == 'left' else '右'}{slot + 1}")
        self.player_name.setObjectName("playerName")
        self.player_name.setFixedHeight(12)
        self.hero = QLabel("等待识别")
        self.hero.setObjectName("heroName")
        self.hero.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        self.state = QLabel("● 未知")
        self.state.setObjectName("lifeState")
        copy.addWidget(self.player_name)
        copy.addWidget(self.hero)
        copy.addWidget(self.state)
        row.addLayout(copy, 1)

    def update_snapshot(self, row: SlotSnapshot) -> None:
        hero = row.hero
        display = HERO_LABELS.get(hero or "", (hero or "等待识别").replace("-", " ").title())
        if row.pending_hero:
            display = f"{display} · 校验"
        self.hero.setText(display)
        state_copy = {
            "alive": "● 存活",
            "dead": "× 已阵亡",
            "unknown": "— 未知",
        }[row.life_state]
        self.state.setText(state_copy)
        if row.player_name:
            self.player_name.setPixmap(QPixmap())
            self.player_name.setText(row.player_name)
        elif row.player_name_image_png:
            name_pixmap = QPixmap()
            if name_pixmap.loadFromData(row.player_name_image_png):
                self.player_name.setText("")
                self.player_name.setPixmap(
                    name_pixmap.scaled(
                        QSize(92, 12),
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )
        confidence = f"英雄 {row.hero_confidence:.0%} / 状态 {row.life_confidence:.0%}"
        self.setToolTip(confidence)
        self.setProperty("life", row.life_state)
        self.style().unpolish(self)
        self.style().polish(self)

        if hero:
            pixmap = load_hero_icon(hero, 34)
            if not pixmap.isNull():
                self.icon.setPixmap(
                    pixmap.scaled(
                        QSize(34, 34),
                        Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )
                return
        self.icon.setPixmap(QPixmap())
        self.icon.setText("?")


class OverlayWindow(QWidget):
    def __init__(self, image_path: Path | None = None) -> None:
        super().__init__()
        self.image_path = image_path
        self.worker: AnalysisWorker | None = None
        self.player_overlay = PlayerTimelineOverlay()
        self.timeline_panel = OverallTimelinePanel()
        self.overlays_enabled = True
        self.drag_origin: QPoint | None = None
        self.expanded_height = 196
        self.setObjectName("overlay")
        self.setWindowTitle("OWCS OB Analyzer")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumWidth(760)
        self.resize(820, self.expanded_height)
        self._build_ui()
        self._apply_style()
        self.hotkeys = GlobalHotkeyPoller(parent=self, window=self)
        self.hotkeys.visibility_pressed.connect(self.toggle_analysis_overlays)
        self.hotkeys.interaction_pressed.connect(self.toggle_overlay_interaction)
        self.hotkeys.status_changed.connect(self._update_hotkey_status)
        self._hotkeys_registered = False
        self.rails.hide()
        self.collapse_button.setText("⌄")
        self.collapse_button.setToolTip("展开选手总览")

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
        self.interaction_button = QPushButton("操作事件栏")
        self.interaction_button.setAccessibleName("切换左侧事件栏鼠标交互模式")
        self.interaction_button.setToolTip("解除事件栏鼠标穿透；也可按 F9")
        self.interaction_button.clicked.connect(self.toggle_overlay_interaction)
        header.addWidget(self.interaction_button)
        self.collapse_button = QPushButton("⌃")
        self.collapse_button.setObjectName("iconButton")
        self.collapse_button.setToolTip("收起选手状态")
        self.collapse_button.setAccessibleName("收起或展开选手状态")
        self.collapse_button.clicked.connect(self.toggle_collapsed)
        header.addWidget(self.collapse_button)
        close = QPushButton("×")
        close.setObjectName("iconButton")
        close.setToolTip("关闭")
        close.setAccessibleName("关闭浮窗")
        close.clicked.connect(self.close)
        header.addWidget(close)
        body.addLayout(header)

        self.rails = QWidget()
        rail_layout = QVBoxLayout(self.rails)
        rail_layout.setContentsMargins(0, 0, 0, 0)
        rail_layout.setSpacing(6)
        self.cards: dict[tuple[str, int], PlayerCard] = {}
        for side, label in (("left", "左侧"), ("right", "右侧")):
            line = QHBoxLayout()
            line.setSpacing(5)
            side_label = QLabel(label)
            side_label.setObjectName("sideLabel")
            side_label.setFixedWidth(34)
            line.addWidget(side_label)
            for slot in range(5):
                card = PlayerCard(side, slot)
                self.cards[(side, slot)] = card
                line.addWidget(card, 1)
            rail_layout.addLayout(line)
        body.addWidget(self.rails)

        footer = QHBoxLayout()
        self.source = QLabel("采集源 · 主显示器顶部 16%")
        self.source.setObjectName("footer")
        footer.addWidget(self.source)
        footer.addStretch(1)
        self.hotkey_hint = QLabel("快捷键等待注册 · 拖动移动")
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
            QLabel#sideLabel {
                color: #9DB0BA; font-family: "Bahnschrift SemiCondensed";
                font-size: 11px; font-weight: 600;
            }
            QFrame#playerCard {
                background: rgba(28, 39, 50, 145);
                border: 1px solid rgba(166, 190, 202, 32);
                border-left: 3px solid #687B85;
                border-radius: 8px;
            }
            QFrame#playerCard[life="alive"] { border-left-color: #72DDB5; }
            QFrame#playerCard[life="dead"] { border-left-color: #FF756F; background: rgba(46, 30, 34, 158); }
            QLabel#heroIcon {
                background: rgba(255, 255, 255, 12);
                border: 1px solid rgba(255, 255, 255, 20); border-radius: 6px;
                color: #71838D; font-family: "Cascadia Mono"; font-weight: 700;
            }
            QLabel#heroName { color: #F3F7F8; font-size: 11px; font-weight: 600; }
            QLabel#playerName { color: #F7FAFB; font-size: 10px; font-weight: 700; }
            QLabel#lifeState { color: #91A3AC; font-size: 10px; }
            QFrame#playerCard[life="alive"] QLabel#lifeState { color: #72DDB5; }
            QFrame#playerCard[life="dead"] QLabel#lifeState { color: #FF8B86; }
            QPushButton {
                min-height: 28px; padding: 0 11px; border-radius: 7px;
                color: #DDE9ED; background: rgba(255, 255, 255, 12);
                border: 1px solid rgba(255, 255, 255, 22);
                font-family: "Segoe UI Variable", "Microsoft YaHei UI"; font-size: 11px;
            }
            QPushButton:hover { background: rgba(255, 255, 255, 24); }
            QPushButton:focus { border-color: #8CE8D0; }
            QPushButton[active="true"] {
                color: #A6F0DE; background: rgba(64, 117, 103, 130);
                border-color: rgba(166, 240, 222, 150); font-weight: 700;
            }
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
        self.timeline_panel.set_interactive(False)
        self.timeline_panel.hide()

    def update_snapshot(self, snapshot: AnalysisSnapshot) -> None:
        self.status_dot.setStyleSheet("color: #72DDB5")
        self.status_text.setText("实时识别中")
        fps = f"{snapshot.capture_fps:.1f} FPS" if snapshot.capture_fps else "采样中"
        self.metrics.setText(
            f"{fps}  ·  本轮 {snapshot.cycle_latency_ms:.0f} ms  ·  英雄复核 {snapshot.classified_slots}/2"
        )
        self.source.setText(f"采集源 · {snapshot.source_name}")
        for row in snapshot.slots:
            self.cards[(row.side, row.slot)].update_snapshot(row)
        self.player_overlay.update_snapshot(snapshot)
        self.timeline_panel.update_snapshot(snapshot)

    def handle_error(self, message: str) -> None:
        self.status_dot.setStyleSheet("color: #FF756F")
        self.status_text.setText("采集失败")
        self.metrics.setText(message)
        self.metrics.setToolTip(message)
        self.player_overlay.hide()
        self.timeline_panel.set_interactive(False)
        self.timeline_panel.hide()

    def toggle_collapsed(self) -> None:
        collapsed = self.rails.isVisible()
        self.rails.setVisible(not collapsed)
        self.collapse_button.setText("⌄" if collapsed else "⌃")
        self.collapse_button.setToolTip("展开选手状态" if collapsed else "收起选手状态")
        self.adjustSize()

    def toggle_analysis_overlays(self) -> None:
        enabled = not self.overlays_enabled
        self.overlays_enabled = enabled
        self.player_overlay.set_enabled(enabled)
        self.timeline_panel.set_enabled(enabled)
        self.timeline_button.setText("隐藏浮层" if enabled else "显示浮层")
        self._sync_interaction_button()

    def toggle_overlay_interaction(self) -> None:
        if not self.overlays_enabled:
            self.overlays_enabled = True
            self.player_overlay.set_enabled(True)
            self.timeline_panel.set_enabled(True)
            self.timeline_button.setText("隐藏浮层")
        interactive = not self.timeline_panel.interactive
        self.timeline_panel.set_interactive(interactive)
        self._sync_interaction_button()
        if interactive:
            self.raise_()
            self.player_overlay.raise_()
            self.timeline_panel.raise_()
            self.timeline_panel.activateWindow()

    def _update_hotkey_status(self, status: str) -> None:
        self.hotkey_hint.setText(f"{status} · 拖动移动")
        self.interaction_button.setToolTip(
            f"解除或恢复事件栏鼠标穿透；{self.hotkeys.interaction_key_label} / Ctrl+Alt+I"
        )

    def _sync_interaction_button(self) -> None:
        interactive = self.timeline_panel.interactive
        self.interaction_button.setText("退出事件操作" if interactive else "操作事件栏")
        self.interaction_button.setProperty("active", "true" if interactive else "false")
        self.interaction_button.style().unpolish(self.interaction_button)
        self.interaction_button.style().polish(self.interaction_button)

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
        self.hotkeys.shutdown()
        self._hotkeys_registered = False
        if self.worker is not None and self.worker.isRunning():
            self.worker.stop()
            self.worker.wait(2500)
        self.player_overlay.close()
        self.timeline_panel.close()
        super().closeEvent(event)
