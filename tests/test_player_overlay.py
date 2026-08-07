from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import cv2
import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from owcs_ob_analyzer.models import AnalysisSnapshot, PlayerEvent, SlotSnapshot
from owcs_ob_analyzer.ui.player_overlay import PlayerTimelineOverlay


def test_player_overlay_is_click_through_transparent_and_hud_anchored() -> None:
    app = QApplication.instance() or QApplication([])
    reference = cv2.imdecode(
        np.fromfile(Path(__file__).parent / "data" / "reference_top.png", dtype=np.uint8),
        cv2.IMREAD_COLOR,
    )
    ok, encoded_name = cv2.imencode(".png", reference[148:178, 56:189])
    assert ok
    slots = tuple(
        SlotSnapshot(
            side=side,
            slot=slot,
            hero="ana",
            hero_confidence=0.8,
            life_state="dead" if slot == 0 else "alive",
            life_confidence=0.9,
            events=(PlayerEvent("death", 12.0, "ana"),),
            player_name_image_png=encoded_name.tobytes(),
        )
        for side in ("left", "right")
        for slot in range(5)
    )
    snapshot = AnalysisSnapshot(
        slots=slots,
        captured_at=12.0,
        cycle_latency_ms=5.0,
        classified_slots=0,
        source_name="test",
        desktop_bounds=(0, 0, 2560, 1600),
    )
    overlay = PlayerTimelineOverlay()
    overlay.update_snapshot(snapshot)
    try:
        assert overlay.windowFlags() & Qt.WindowType.WindowTransparentForInput
        assert overlay.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        assert "rgba(7, 13, 20, 76)" in overlay.styleSheet()
        first = overlay.cards[("left", 0)]
        assert first.x() >= 56
        assert first.y() == 214
        assert first.height() == 76
        assert first.icon.pixmap() is not None and not first.icon.pixmap().isNull()
        assert first.player_name.pixmap() is not None and not first.player_name.pixmap().isNull()
        assert "00:12 阵亡" in first.events.text()
        overlay.set_enabled(False)
        assert not overlay.isVisible()
        overlay.update_snapshot(snapshot)
        assert not overlay.isVisible()
    finally:
        overlay.close()
        app.processEvents()
