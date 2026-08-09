from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from owcs_ob_analyzer.models import AnalysisSnapshot, SlotSnapshot, TimelineEvent
from owcs_ob_analyzer.ui import hotkeys as hotkey_module
from owcs_ob_analyzer.ui import overlay as overlay_module
from owcs_ob_analyzer.ui.hotkeys import GlobalHotkeyPoller
from owcs_ob_analyzer.ui.overlay import OverlayWindow
from owcs_ob_analyzer.ui.timeline_panel import OverallTimelinePanel


def _snapshot() -> AnalysisSnapshot:
    return AnalysisSnapshot(
        slots=tuple(
            SlotSnapshot(
                side=side,
                slot=slot,
                hero=None,
                hero_confidence=0.0,
                life_state="unknown",
                life_confidence=0.0,
                player_name=(
                    "PLAYER"
                    if (side, slot) == ("left", 0)
                    else "测试选手"
                    if (side, slot) == ("right", 2)
                    else None
                ),
            )
            for side in ("left", "right")
            for slot in range(5)
        ),
        captured_at=72.0,
        cycle_latency_ms=4.0,
        classified_slots=0,
        source_name="test",
        desktop_bounds=(0, 0, 2560, 1600),
        elapsed_s=72.0,
        timeline=(
            TimelineEvent("left", 0, "hero_pick", 1.0, "ana"),
            TimelineEvent("right", 2, "death", 31.0, "moira"),
            TimelineEvent("left", 0, "hero_switch", 51.0, "tracer", "ana"),
            TimelineEvent("right", 2, "respawn", 68.0, "moira"),
        ),
    )


def test_overall_timeline_filters_and_remains_clickable() -> None:
    app = QApplication.instance() or QApplication([])
    panel = OverallTimelinePanel()
    panel.update_snapshot(_snapshot())
    try:
        assert panel.current_time.text() == "01:12"
        assert panel.event_list.count() == 4
        assert panel.event_list.item(0).text().startswith("01:08  测试选手  复活")
        assert panel.player_filter.itemText(1) == "PLAYER"
        assert panel.player_filter.itemText(8) == "测试选手"
        assert panel.interactive
        assert not panel.windowFlags() & Qt.WindowType.WindowTransparentForInput
        assert not panel.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus
        assert not panel.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

        panel.player_filter.setCurrentIndex(1)
        assert panel.event_list.count() == 2
        panel.event_filter.setCurrentIndex(panel.event_filter.findData("hero_switch"))
        assert panel.event_list.count() == 1
        assert "切换至 Tracer" in panel.event_list.item(0).text()

        panel.set_enabled(False)
        assert panel.interactive
        assert not panel.windowFlags() & Qt.WindowType.WindowTransparentForInput
        panel.set_enabled(True)
        assert panel.interactive
    finally:
        panel.close()
        app.processEvents()


def test_global_hotkey_chords_emit_only_on_key_down_edges() -> None:
    app = QApplication.instance() or QApplication([])
    state: dict[int, bool] = {}
    poller = GlobalHotkeyPoller(lambda key: state.get(key, False))
    poller.timer.stop()
    seen: list[str] = []
    poller.visibility_pressed.connect(lambda: seen.append("visibility"))
    poller.foreground_pressed.connect(lambda: seen.append("foreground"))

    state[poller.VK_CONTROL] = True
    state[poller.VK_ALT] = True
    state[poller.VK_H] = True
    poller._poll()
    poller._poll()
    state[poller.VK_H] = False
    poller._poll()
    state[poller.VK_H] = True
    state[poller.VK_I] = True
    poller._poll()

    assert seen == ["visibility", "visibility", "foreground"]
    app.processEvents()


def test_bare_function_keys_are_not_observed() -> None:
    app = QApplication.instance() or QApplication([])
    state = {0x77: True, 0x78: True}  # F8 and F9
    poller = GlobalHotkeyPoller(lambda key: state.get(key, False))
    poller.timer.stop()
    seen: list[str] = []
    poller.visibility_pressed.connect(lambda: seen.append("visibility"))
    poller.foreground_pressed.connect(lambda: seen.append("foreground"))

    poller._poll()

    assert seen == []
    app.processEvents()


def test_windows_registration_contains_only_the_two_letter_chords() -> None:
    app = QApplication.instance() or QApplication([])
    poller = GlobalHotkeyPoller(lambda _key: False)
    poller.timer.stop()
    assert poller.bindings() == (
        (
            poller.ID_VISIBILITY_CHORD,
            hotkey_module.MOD_CONTROL | hotkey_module.MOD_ALT | hotkey_module.MOD_NOREPEAT,
            poller.VK_H,
        ),
        (
            poller.ID_FOREGROUND_CHORD,
            hotkey_module.MOD_CONTROL | hotkey_module.MOD_ALT | hotkey_module.MOD_NOREPEAT,
            poller.VK_I,
        ),
    )
    app.processEvents()


def test_global_hotkey_alternative_chords_and_native_poll_deduplication() -> None:
    app = QApplication.instance() or QApplication([])
    state: dict[int, bool] = {}
    poller = GlobalHotkeyPoller(lambda key: state.get(key, False))
    poller.timer.stop()
    seen: list[str] = []
    poller.visibility_pressed.connect(lambda: seen.append("visibility"))
    poller.foreground_pressed.connect(lambda: seen.append("foreground"))

    state[poller.VK_CONTROL] = True
    state[poller.VK_ALT] = True
    state[poller.VK_H] = True
    poller._poll()
    poller._dispatch_registered_hotkey(poller.ID_VISIBILITY_CHORD)
    state[poller.VK_H] = False
    poller._poll()
    state[poller.VK_I] = True
    poller._poll()

    assert seen == ["visibility", "foreground"]
    app.processEvents()


def test_overlay_foreground_action_never_disables_timeline_clicks(monkeypatch) -> None:
    app = QApplication.instance() or QApplication([])
    window = OverlayWindow()
    window.hotkeys.timer.stop()
    brought_to_front = []
    monkeypatch.setattr(
        "owcs_ob_analyzer.ui.overlay.bring_to_front",
        lambda target: brought_to_front.append(target) or True,
    )
    try:
        window.hotkeys.visibility_pressed.emit()
        assert not window.overlays_enabled
        assert not window.player_overlay.enabled
        assert not window.timeline_panel.enabled

        window.hotkeys.foreground_pressed.emit()
        assert window.overlays_enabled
        assert window.timeline_panel.interactive
        assert brought_to_front == [window]
        window.hotkeys.foreground_pressed.emit()
        assert window.timeline_panel.interactive
        assert brought_to_front == [window, window]
        assert not hasattr(window, "foreground_button")
        assert not hasattr(window, "collapse_button")
        assert not hasattr(window, "rails")
        assert not hasattr(window, "cards")
        assert window.hotkey_hint.text() == (
            "Ctrl+Alt+H 显示/隐藏浮层 · Ctrl+Alt+I 重新置顶 · 拖动移动"
        )
    finally:
        window.close()
        app.processEvents()


def test_close_button_stops_worker_closes_overlays_and_quits(monkeypatch) -> None:
    app = QApplication.instance() or QApplication([])
    window = OverlayWindow()
    window.hotkeys.timer.stop()

    class Worker:
        stopped = False
        waits: list[int] = []

        @staticmethod
        def isRunning() -> bool:
            return True

        def stop(self) -> None:
            self.stopped = True

        def wait(self, timeout: int) -> bool:
            self.waits.append(timeout)
            return True

    class App:
        quit_count = 0

        def quit(self) -> None:
            self.quit_count += 1

    fake_app = App()

    class ApplicationFacade:
        @staticmethod
        def instance():
            return fake_app

    class TimerFacade:
        @staticmethod
        def singleShot(_delay: int, callback) -> None:
            callback()

    worker = Worker()
    window.worker = worker
    monkeypatch.setattr(overlay_module, "QApplication", ApplicationFacade)
    monkeypatch.setattr(overlay_module, "QTimer", TimerFacade)

    window.close_button.click()

    assert window._closing
    assert worker.stopped
    assert worker.waits == [5000]
    assert window.worker is None
    assert not window.player_overlay.isVisible()
    assert not window.timeline_panel.isVisible()
    assert fake_app.quit_count == 1
    window.deleteLater()
    app.processEvents()
