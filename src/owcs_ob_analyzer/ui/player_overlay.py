from __future__ import annotations

from functools import lru_cache

from PySide6.QtCore import QPoint, QSize, Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QApplication, QFrame, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from ..analyzer import resource_root
from ..config import DEFAULT_LAYOUT
from ..models import AnalysisSnapshot, PlayerEvent, SlotSnapshot


def _clock(seconds: float) -> str:
    total = max(0, round(seconds))
    return f"{total // 60:02d}:{total % 60:02d}"


def _hero_name(hero: str | None) -> str:
    return (hero or "等待英雄").replace("-", " ").title()


def _event_copy(event: PlayerEvent) -> str:
    stamp = _clock(event.elapsed_s)
    if event.kind == "hero_pick":
        return f"{stamp} 识别 {_hero_name(event.hero)}"
    if event.kind == "hero_switch":
        return f"{stamp} → {_hero_name(event.hero)}"
    if event.kind == "death":
        return f"{stamp} 阵亡"
    return f"{stamp} 复活"


@lru_cache(maxsize=128)
def load_hero_icon(hero: str | None, size: int) -> QPixmap:
    if not hero:
        return QPixmap()
    pixmap = QPixmap(str(resource_root() / "heroes" / "illustrated" / f"{hero}.png"))
    if pixmap.isNull():
        return pixmap
    return pixmap.scaled(
        QSize(size, size),
        Qt.AspectRatioMode.KeepAspectRatioByExpanding,
        Qt.TransformationMode.SmoothTransformation,
    )


class TimelinePlayerCard(QFrame):
    def __init__(self, side: str, slot: int, parent: QWidget) -> None:
        super().__init__(parent)
        self.side = side
        self.slot = slot
        self.setObjectName("timelineCard")
        self.setProperty("life", "unknown")
        self.setAccessibleName(f"{side} {slot + 1}号选手实时事件")

        body = QVBoxLayout(self)
        body.setContentsMargins(5, 4, 5, 4)
        body.setSpacing(2)

        identity = QHBoxLayout()
        identity.setSpacing(4)
        self.icon = QLabel("?")
        self.icon.setObjectName("timelineIcon")
        self.icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.icon.setFixedSize(22, 22)
        identity.addWidget(self.icon)

        identity_copy = QVBoxLayout()
        identity_copy.setSpacing(0)
        self.player_name = QLabel(f"{'左' if side == 'left' else '右'}{slot + 1}")
        self.player_name.setObjectName("timelinePlayerName")
        self.player_name.setFixedHeight(12)
        self.hero = QLabel("等待英雄")
        self.hero.setObjectName("timelineHero")
        identity_copy.addWidget(self.player_name)
        identity_copy.addWidget(self.hero)
        identity.addLayout(identity_copy, 1)
        body.addLayout(identity)

        self.state = QLabel("— 未知")
        self.state.setObjectName("timelineState")
        self.events = QLabel("尚无事件")
        self.events.setObjectName("timelineEvents")
        self.events.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        body.addWidget(self.state)
        body.addWidget(self.events, 1)

    def update_snapshot(self, row: SlotSnapshot) -> None:
        self.hero.setText(_hero_name(row.hero))
        self.state.setText(
            {"alive": "● 存活", "dead": "× 阵亡", "unknown": "— 未知"}[row.life_state]
        )
        recent = row.events[-2:]
        self.events.setText("\n".join(_event_copy(event) for event in recent) if recent else "尚无事件")

        hero_pixmap = load_hero_icon(row.hero, 22)
        if hero_pixmap.isNull():
            self.icon.setPixmap(QPixmap())
            self.icon.setText("?")
        else:
            self.icon.setText("")
            self.icon.setPixmap(hero_pixmap)

        if row.player_name:
            self.player_name.setPixmap(QPixmap())
            self.player_name.setText(row.player_name)
        elif row.player_name_image_png:
            name_pixmap = QPixmap()
            if name_pixmap.loadFromData(row.player_name_image_png):
                max_width = max(32, self.width() - 38)
                self.player_name.setText("")
                self.player_name.setPixmap(
                    name_pixmap.scaled(
                        QSize(max_width, 12),
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )

        self.setProperty("life", row.life_state)
        self.style().unpolish(self)
        self.style().polish(self)


class PlayerTimelineOverlay(QWidget):
    """Mouse-transparent player identities and timelines anchored below the game HUD."""

    def __init__(self) -> None:
        super().__init__()
        self.enabled = True
        self.setWindowTitle("OWCS Player Timelines")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowTransparentForInput
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.cards = {
            (side, slot): TimelinePlayerCard(side, slot, self)
            for side in ("left", "right")
            for slot in range(5)
        }
        self.setStyleSheet(
            """
            QWidget { background: transparent; }
            QFrame#timelineCard {
                background: rgba(7, 13, 20, 76);
                border: 1px solid rgba(218, 235, 242, 46);
                border-top: 2px solid rgba(139, 160, 170, 150);
                border-radius: 5px;
            }
            QFrame#timelineCard[life="alive"] { border-top-color: rgba(114, 221, 181, 205); }
            QFrame#timelineCard[life="dead"] {
                background: rgba(35, 10, 15, 88);
                border-top-color: rgba(255, 117, 111, 220);
            }
            QLabel {
                color: #F2F7F8;
                background: transparent;
                font-family: "Segoe UI Variable", "Microsoft YaHei UI";
            }
            QLabel#timelineIcon {
                background: rgba(255, 255, 255, 15);
                border: 1px solid rgba(255, 255, 255, 24);
                border-radius: 4px;
                color: #80929B;
                font-weight: 700;
            }
            QLabel#timelinePlayerName { font-size: 8px; font-weight: 700; }
            QLabel#timelineHero { font-size: 8px; font-weight: 600; color: #D8E3E7; }
            QLabel#timelineState { color: #C7D4DA; font-size: 8px; font-weight: 600; }
            QFrame#timelineCard[life="alive"] QLabel#timelineState { color: #91E8C7; }
            QFrame#timelineCard[life="dead"] QLabel#timelineState { color: #FFAAA6; }
            QLabel#timelineEvents { color: #D4DFE3; font-size: 8px; }
            """
        )

    def update_snapshot(self, snapshot: AnalysisSnapshot) -> None:
        self._sync_geometry(snapshot)
        for row in snapshot.slots:
            self.cards[(row.side, row.slot)].update_snapshot(row)
        if self.enabled and not self.isVisible():
            self.show()

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        if not enabled:
            self.hide()

    def _sync_geometry(self, snapshot: AnalysisSnapshot) -> None:
        primary = QApplication.primaryScreen()
        dpr = max(1.0, primary.devicePixelRatio())
        if snapshot.desktop_bounds is None:
            screen_geometry = primary.geometry()
            left, top = screen_geometry.left(), screen_geometry.top()
            width, height = screen_geometry.width(), screen_geometry.height()
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
        geometry = (left, top, width, height)
        if (self.x(), self.y(), self.width(), self.height()) != geometry:
            self.setGeometry(*geometry)

        hud_height = min(physical_height, round(physical_width / DEFAULT_LAYOUT.logical_aspect_ratio)) / dpr
        for side in ("left", "right"):
            for slot, box in enumerate(DEFAULT_LAYOUT.status_rois[side]):
                x1, _, x2, y2 = box
                card_left = round(x1 * physical_width / dpr) + 1
                card_top = round(y2 * hud_height) + 4
                card_width = max(76, round((x2 - x1) * physical_width / dpr) - 3)
                self.cards[(side, slot)].setGeometry(card_left, card_top, card_width, 76)
