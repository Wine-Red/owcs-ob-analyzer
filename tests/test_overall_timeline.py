from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from owcs_ob_analyzer.models import AnalysisSnapshot, TimelineEvent
from owcs_ob_analyzer.ui import hotkeys as hotkey_module
from owcs_ob_analyzer.ui.hotkeys import GlobalHotkeyPoller
from owcs_ob_analyzer.ui.overlay import OverlayWindow
from owcs_ob_analyzer.ui.timeline_panel import OverallTimelinePanel


def _snapshot() -> AnalysisSnapshot:
    return AnalysisSnapshot(
        slots=(),
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


def test_overall_timeline_filters_and_interaction_mode() -> None:
    app = QApplication.instance() or QApplication([])
    panel = OverallTimelinePanel()
    panel.update_snapshot(_snapshot())
    try:
        assert panel.current_time.text() == "01:12"
        assert panel.event_list.count() == 4
        assert panel.event_list.item(0).text().startswith("01:08  右3  复活")
        assert panel.windowFlags() & Qt.WindowType.WindowTransparentForInput

        panel.player_filter.setCurrentIndex(1)
        assert panel.event_list.count() == 2
        panel.event_filter.setCurrentIndex(panel.event_filter.findData("hero_switch"))
        assert panel.event_list.count() == 1
        assert "切换至 Tracer" in panel.event_list.item(0).text()

        panel.set_interactive(True)
        assert panel.interactive
        assert not panel.windowFlags() & Qt.WindowType.WindowTransparentForInput
        assert not panel.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus
        assert not panel.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        panel.set_interactive(False)
        assert panel.windowFlags() & Qt.WindowType.WindowTransparentForInput
        assert panel.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus
        assert panel.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
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
    poller.interaction_pressed.connect(lambda: seen.append("interaction"))

    state[poller.VK_CONTROL] = True
    state[poller.VK_ALT] = True
    state[poller.VK_O] = True
    poller._poll()
    poller._poll()
    state[poller.VK_O] = False
    poller._poll()
    state[poller.VK_O] = True
    state[poller.VK_I] = True
    poller._poll()

    assert seen == ["visibility", "visibility", "interaction"]
    app.processEvents()


def test_bare_function_keys_are_not_observed() -> None:
    app = QApplication.instance() or QApplication([])
    state = {0x77: True, 0x78: True}  # F8 and F9
    poller = GlobalHotkeyPoller(lambda key: state.get(key, False))
    poller.timer.stop()
    seen: list[str] = []
    poller.visibility_pressed.connect(lambda: seen.append("visibility"))
    poller.interaction_pressed.connect(lambda: seen.append("interaction"))

    poller._poll()

    assert seen == []
    app.processEvents()


def test_windows_registration_contains_only_the_two_letter_chords(monkeypatch) -> None:
    class ApiCall:
        def __init__(self, result=True) -> None:
            self.result = result
            self.calls = []

        def __call__(self, *args):
            self.calls.append(args)
            return self.result

    class User32:
        RegisterHotKey = ApiCall()
        UnregisterHotKey = ApiCall()

    class Windll:
        user32 = User32()

    class NativeWindow:
        @staticmethod
        def winId() -> int:
            return 1234

    class NativeApp:
        def installNativeEventFilter(self, _event_filter) -> None:
            pass

        def removeNativeEventFilter(self, _event_filter) -> None:
            pass

    native_app = NativeApp()

    class ApplicationFacade:
        @staticmethod
        def instance():
            return native_app

    app = QApplication.instance() or QApplication([])
    monkeypatch.setattr(hotkey_module.sys, "platform", "win32")
    monkeypatch.setattr(hotkey_module.ctypes, "windll", Windll())
    monkeypatch.setattr(hotkey_module, "QApplication", ApplicationFacade)
    monkeypatch.setattr(
        GlobalHotkeyPoller,
        "_create_key_reader",
        staticmethod(lambda: lambda _key: False),
    )
    poller = GlobalHotkeyPoller(window=NativeWindow())
    poller.timer.stop()
    try:
        poller.register()
        registered = [
            (int(hotkey_id), int(modifiers), int(virtual_key))
            for _hwnd, hotkey_id, modifiers, virtual_key in Windll.user32.RegisterHotKey.calls
        ]
        assert registered == [
            (
                poller.ID_VISIBILITY_CHORD,
                hotkey_module.MOD_CONTROL | hotkey_module.MOD_ALT | hotkey_module.MOD_NOREPEAT,
                poller.VK_O,
            ),
            (
                poller.ID_INTERACTION_CHORD,
                hotkey_module.MOD_CONTROL | hotkey_module.MOD_ALT | hotkey_module.MOD_NOREPEAT,
                poller.VK_I,
            ),
        ]
        assert poller._registered_ids == {
            poller.ID_VISIBILITY_CHORD,
            poller.ID_INTERACTION_CHORD,
        }
    finally:
        poller.shutdown()
        app.processEvents()


def test_global_hotkey_alternative_chords_and_native_poll_deduplication() -> None:
    app = QApplication.instance() or QApplication([])
    state: dict[int, bool] = {}
    poller = GlobalHotkeyPoller(lambda key: state.get(key, False))
    poller.timer.stop()
    seen: list[str] = []
    poller.visibility_pressed.connect(lambda: seen.append("visibility"))
    poller.interaction_pressed.connect(lambda: seen.append("interaction"))

    state[poller.VK_CONTROL] = True
    state[poller.VK_ALT] = True
    state[poller.VK_O] = True
    poller._poll()
    poller._dispatch_registered_hotkey(poller.ID_VISIBILITY_CHORD)
    state[poller.VK_O] = False
    poller._poll()
    state[poller.VK_I] = True
    poller._poll()

    assert seen == ["visibility", "interaction"]
    app.processEvents()


def test_overlay_connects_global_hotkeys_to_visibility_and_interaction(monkeypatch) -> None:
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

        window.hotkeys.interaction_pressed.emit()
        assert window.overlays_enabled
        assert window.timeline_panel.interactive
        assert brought_to_front == [window]
        window.hotkeys.interaction_pressed.emit()
        assert not window.timeline_panel.interactive

        window.interaction_button.click()
        assert window.overlays_enabled
        assert window.timeline_panel.interactive
        assert brought_to_front == [window, window]
        assert window.interaction_button.text() == "退出事件操作"
        window.interaction_button.click()
        assert not window.timeline_panel.interactive
        assert window.interaction_button.text() == "操作事件栏"
    finally:
        window.close()
        app.processEvents()
