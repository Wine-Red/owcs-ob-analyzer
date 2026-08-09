from __future__ import annotations

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QVBoxLayout,
    QWidget,
)

from ..config import DEFAULT_LAYOUT
from ..models import AnalysisSnapshot, TimelineEvent
from .player_overlay import _clock, _hero_name


EVENT_OPTIONS = (
    ("全部事件", None),
    ("英雄确认", "hero_pick"),
    ("英雄切换", "hero_switch"),
    ("阵亡", "death"),
    ("复活", "respawn"),
)


def _player_label(side: str, slot: int) -> str:
    return f"{'左' if side == 'left' else '右'}{slot + 1}"


def _timeline_copy(event: TimelineEvent, player_name: str | None = None) -> str:
    player = player_name or _player_label(event.side, event.slot)
    if event.kind == "hero_pick":
        detail = f"确认英雄 {_hero_name(event.hero)}"
    elif event.kind == "hero_switch":
        detail = f"切换至 {_hero_name(event.hero)}"
    elif event.kind == "death":
        detail = "阵亡"
    else:
        detail = "复活"
    return f"{_clock(event.elapsed_s)}  {player}  {detail}"


class OverallTimelinePanel(QWidget):
    """High-transparency match timeline that remains directly interactive."""

    def __init__(self) -> None:
        super().__init__()
        self.enabled = True
        self.interactive = True
        self._snapshot: AnalysisSnapshot | None = None
        self._timeline_signature: tuple[object, ...] | None = None
        self.setWindowTitle("OWCS Overall Timeline")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setProperty("interactive", "true")
        self._build_ui()
        self._apply_style()

    def _build_ui(self) -> None:
        shell = QFrame(self)
        shell.setObjectName("timelineShell")
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.addWidget(shell)

        body = QVBoxLayout(shell)
        body.setContentsMargins(11, 10, 11, 11)
        body.setSpacing(8)

        header = QHBoxLayout()
        title = QLabel("整体时间线")
        title.setObjectName("timelineTitle")
        header.addWidget(title)
        header.addStretch(1)
        self.current_time = QLabel("00:00")
        self.current_time.setObjectName("currentTime")
        header.addWidget(self.current_time)
        body.addLayout(header)

        self.mode_hint = QLabel("可直接点击筛选 · Ctrl+Alt+I 重新置顶")
        self.mode_hint.setObjectName("modeHint")
        body.addWidget(self.mode_hint)

        filters = QHBoxLayout()
        filters.setSpacing(6)
        self.player_filter = QComboBox()
        self.player_filter.setAccessibleName("按选手筛选整体时间线")
        self.player_filter.addItem("全部选手", None)
        for side in ("left", "right"):
            for slot in range(5):
                self.player_filter.addItem(_player_label(side, slot), (side, slot))
        filters.addWidget(self.player_filter, 1)

        self.event_filter = QComboBox()
        self.event_filter.setAccessibleName("按事件类型筛选整体时间线")
        for label, kind in EVENT_OPTIONS:
            self.event_filter.addItem(label, kind)
        filters.addWidget(self.event_filter, 1)
        body.addLayout(filters)

        self.event_list = QListWidget()
        self.event_list.setObjectName("overallEvents")
        self.event_list.setAccessibleName("整体时间线事件，最新事件在上")
        self.event_list.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        body.addWidget(self.event_list, 1)

        self.empty_hint = QLabel("等待识别事件")
        self.empty_hint.setObjectName("emptyHint")
        self.empty_hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
        body.addWidget(self.empty_hint)

        self.player_filter.currentIndexChanged.connect(self._render_events)
        self.event_filter.currentIndexChanged.connect(self._render_events)

    def _apply_style(self) -> None:
        self.setStyleSheet(
            """
            QWidget { background: transparent; }
            QFrame#timelineShell {
                background: rgba(7, 13, 20, 86);
                border: 1px solid rgba(211, 231, 239, 54);
                border-left: 2px solid rgba(140, 232, 208, 175);
                border-radius: 9px;
            }
            QLabel {
                color: #EAF2F5;
                background: transparent;
                font-family: "Segoe UI Variable", "Microsoft YaHei UI";
            }
            QLabel#timelineTitle { font-size: 13px; font-weight: 700; }
            QLabel#currentTime {
                color: #A6F0DE;
                font-family: "Cascadia Mono", "Microsoft YaHei UI";
                font-size: 14px;
                font-weight: 700;
            }
            QLabel#modeHint { color: #AABBC3; font-size: 10px; }
            QLabel#modeHint { color: #A6F0DE; font-weight: 700; }
            QComboBox {
                min-height: 28px;
                padding: 0 7px;
                color: #E9F1F4;
                background: rgba(22, 34, 44, 190);
                border: 1px solid rgba(207, 228, 237, 55);
                border-radius: 5px;
                font-family: "Segoe UI Variable", "Microsoft YaHei UI";
                font-size: 10px;
            }
            QComboBox:focus { border-color: #8CE8D0; }
            QComboBox QAbstractItemView {
                color: #EFF5F7;
                background: #111B24;
                selection-background-color: #24483F;
            }
            QListWidget#overallEvents {
                color: #DDE8EC;
                background: rgba(8, 14, 21, 64);
                border: 1px solid rgba(207, 228, 237, 32);
                border-radius: 6px;
                outline: none;
                padding: 3px;
                font-family: "Segoe UI Variable", "Microsoft YaHei UI";
                font-size: 10px;
            }
            QListWidget#overallEvents::item { min-height: 25px; padding: 2px 4px; }
            QListWidget#overallEvents::item:selected { background: rgba(92, 170, 148, 105); }
            QLabel#emptyHint { color: #82949D; font-size: 10px; }
            """
        )

    def update_snapshot(self, snapshot: AnalysisSnapshot) -> None:
        self._snapshot = snapshot
        self._sync_geometry(snapshot)
        self.current_time.setText(_clock(snapshot.elapsed_s))
        names = tuple(row.player_name for row in snapshot.slots)
        signature = (*snapshot.timeline, *names)
        if signature != self._timeline_signature:
            self._timeline_signature = signature
            for index, row in enumerate(snapshot.slots, start=1):
                label = row.player_name or _player_label(row.side, row.slot)
                self.player_filter.setItemText(index, label)
            self._render_events()
        if self.enabled and not self.isVisible():
            self.show()

    def _render_events(self, *_args: object) -> None:
        if self._snapshot is None:
            return
        selected_player = self.player_filter.currentData()
        selected_kind = self.event_filter.currentData()
        events = [
            event
            for event in reversed(self._snapshot.timeline)
            if (selected_player is None or (event.side, event.slot) == selected_player)
            and (selected_kind is None or event.kind == selected_kind)
        ]
        self.event_list.clear()
        player_names = {
            (row.side, row.slot): row.player_name
            for row in self._snapshot.slots
        }
        for event in events:
            item = QListWidgetItem(
                _timeline_copy(event, player_names.get((event.side, event.slot)))
            )
            if event.kind == "death":
                item.setForeground(QColor("#FFAAA6"))
            elif event.kind == "respawn":
                item.setForeground(QColor("#91E8C7"))
            self.event_list.addItem(item)
        self.empty_hint.setVisible(not events)
        self.event_list.setVisible(bool(events))

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        if not enabled:
            self.hide()
        elif self._snapshot is not None:
            self.show()

    def _sync_geometry(self, snapshot: AnalysisSnapshot) -> None:
        primary = QApplication.primaryScreen()
        dpr = max(1.0, primary.devicePixelRatio())
        if snapshot.desktop_bounds is None:
            screen = primary.geometry()
            left, top, width, height = screen.left(), screen.top(), screen.width(), screen.height()
            physical_width = round(width * dpr)
            physical_height = round(height * dpr)
        else:
            physical_left, physical_top, physical_right, physical_bottom = snapshot.desktop_bounds
            candidate = QApplication.screenAt(
                QPoint(round(physical_left / dpr), round(physical_top / dpr))
            )
            if candidate is not None:
                primary = candidate
                dpr = max(1.0, primary.devicePixelRatio())
            left = round(physical_left / dpr)
            top = round(physical_top / dpr)
            physical_width = physical_right - physical_left
            physical_height = physical_bottom - physical_top
            width = round(physical_width / dpr)
            height = round(physical_height / dpr)

        hud_height = min(
            physical_height,
            round(physical_width / DEFAULT_LAYOUT.logical_aspect_ratio),
        ) / dpr
        player_cards_bottom = round(0.1460 * hud_height) + 4 + 76
        panel_top = top + player_cards_bottom + 16
        panel_width = min(282, max(238, round(width * 0.17)))
        panel_height = min(460, max(250, top + height - panel_top - 24))
        geometry = (left + 16, panel_top, panel_width, panel_height)
        if (self.x(), self.y(), self.width(), self.height()) != geometry:
            self.setGeometry(*geometry)
