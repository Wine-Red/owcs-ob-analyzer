from __future__ import annotations

from collections.abc import Callable
import ctypes
from ctypes import wintypes
import sys
import weakref

from PySide6.QtCore import QAbstractNativeEventFilter, QObject, QTimer, Signal
from PySide6.QtWidgets import QApplication, QWidget


WM_HOTKEY = 0x0312
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_NOREPEAT = 0x4000


class _WindowsHotkeyEventFilter(QAbstractNativeEventFilter):
    def __init__(self, owner: GlobalHotkeyPoller) -> None:
        super().__init__()
        self._owner = weakref.ref(owner)

    def nativeEventFilter(self, event_type, message):
        try:
            event_name = bytes(event_type)
        except TypeError:
            event_name = event_type
        if event_name not in (b"windows_generic_MSG", b"windows_dispatcher_MSG"):
            return False, 0
        try:
            native_message = wintypes.MSG.from_address(int(message))
        except (TypeError, ValueError):
            return False, 0
        if native_message.message != WM_HOTKEY:
            return False, 0
        owner = self._owner()
        if owner is None:
            return False, 0
        return owner._dispatch_registered_hotkey(int(native_message.wParam)), 0


class GlobalHotkeyPoller(QObject):
    """Register Win32 hotkeys, with read-only key polling as a visible fallback."""

    visibility_pressed = Signal()
    interaction_pressed = Signal()
    status_changed = Signal(str)

    VK_F8 = 0x77
    VK_F9 = 0x78
    VK_F10 = 0x79
    VK_CONTROL = 0x11
    VK_ALT = 0x12
    VK_I = 0x49
    VK_O = 0x4F
    ID_F8 = 0xB801
    ID_F9 = 0xB802
    ID_VISIBILITY_CHORD = 0xB803
    ID_INTERACTION_CHORD = 0xB804

    def __init__(
        self,
        key_reader: Callable[[int], bool] | None = None,
        parent: QObject | None = None,
        window: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.window = window
        self.interaction_virtual_key = self.VK_F9
        self.interaction_key_label = "F9"
        self._explicit_key_reader = key_reader is not None
        self._key_reader = key_reader or self._create_key_reader()
        self._action_down = {"visibility": False, "interaction": False}
        self._poll_enabled = self._explicit_key_reader
        self._registered_ids: set[int] = set()
        self._native_filter: _WindowsHotkeyEventFilter | None = None
        self._window_handle: int | None = None
        self.available = self._key_reader is not None
        self.registration_summary = "快捷键等待注册"

        self.timer = QTimer(self)
        self.timer.setInterval(45)
        self.timer.timeout.connect(self._poll)
        if self._poll_enabled:
            self.timer.start()

    def register(self) -> None:
        if self._explicit_key_reader:
            self.registration_summary = "快捷键测试监听"
            self.status_changed.emit(self.registration_summary)
            return
        if sys.platform != "win32" or self.window is None:
            self._enable_polling_fallback()
            self.registration_summary = "快捷键使用兼容监听"
            self.status_changed.emit(self.registration_summary)
            return

        self.shutdown()
        self.interaction_virtual_key = self.VK_F9
        self.interaction_key_label = "F9"
        app = QApplication.instance()
        if app is None:
            self._enable_polling_fallback()
            self.registration_summary = "快捷键使用兼容监听"
            self.status_changed.emit(self.registration_summary)
            return

        self._window_handle = int(self.window.winId())
        self._native_filter = _WindowsHotkeyEventFilter(self)
        app.installNativeEventFilter(self._native_filter)

        user32 = ctypes.windll.user32
        user32.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
        user32.RegisterHotKey.restype = wintypes.BOOL
        if user32.RegisterHotKey(
            wintypes.HWND(self._window_handle),
            self.ID_F8,
            MOD_NOREPEAT,
            self.VK_F8,
        ):
            self._registered_ids.add(self.ID_F8)

        interaction_registered = user32.RegisterHotKey(
            wintypes.HWND(self._window_handle),
            self.ID_F9,
            MOD_NOREPEAT,
            self.VK_F9,
        )
        if interaction_registered:
            self._registered_ids.add(self.ID_F9)
        else:
            self.interaction_virtual_key = self.VK_F10
            self.interaction_key_label = "F10"
            if user32.RegisterHotKey(
                wintypes.HWND(self._window_handle),
                self.ID_F9,
                MOD_NOREPEAT,
                self.VK_F10,
            ):
                self._registered_ids.add(self.ID_F9)

        # Function keys on many laptops are translated into firmware/media keys.
        # Keep ordinary-letter chords as reliable alternatives and poll all
        # bindings even when RegisterHotKey succeeds.  The shared edge state
        # prevents a native WM_HOTKEY and the polling fallback from firing twice.
        for hotkey_id, modifiers, virtual_key in (
            (self.ID_VISIBILITY_CHORD, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, self.VK_O),
            (self.ID_INTERACTION_CHORD, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, self.VK_I),
        ):
            if user32.RegisterHotKey(
                wintypes.HWND(self._window_handle),
                hotkey_id,
                modifiers,
                virtual_key,
            ):
                self._registered_ids.add(hotkey_id)

        self._enable_polling_fallback()
        self.available = bool(self._registered_ids) or self._poll_enabled
        primary_registered = {
            self.ID_F8,
            self.ID_F9,
        }.issubset(self._registered_ids)
        if primary_registered:
            suffix = "（F9 被占用）" if self.interaction_key_label == "F10" else ""
            self.registration_summary = (
                f"F8 / Ctrl+Alt+O 显隐 · "
                f"{self.interaction_key_label} / Ctrl+Alt+I 交互{suffix}"
            )
        elif self._registered_ids or self._poll_enabled:
            self.registration_summary = (
                f"F8 / Ctrl+Alt+O 显隐 · "
                f"{self.interaction_key_label} / Ctrl+Alt+I 交互"
            )
        else:
            self.registration_summary = f"F8 / {self.interaction_key_label} 不可用"
        self.status_changed.emit(self.registration_summary)

    def shutdown(self) -> None:
        if sys.platform == "win32" and self._window_handle is not None:
            user32 = ctypes.windll.user32
            for hotkey_id in tuple(self._registered_ids):
                user32.UnregisterHotKey(wintypes.HWND(self._window_handle), hotkey_id)
        self._registered_ids.clear()
        app = QApplication.instance()
        if app is not None and self._native_filter is not None:
            app.removeNativeEventFilter(self._native_filter)
        self._native_filter = None
        self._window_handle = None
        self._action_down = {"visibility": False, "interaction": False}
        if not self._explicit_key_reader:
            self._poll_enabled = False
            self.timer.stop()

    def _enable_polling_fallback(self) -> None:
        if self._key_reader is None:
            return
        self._poll_enabled = True
        if not self.timer.isActive():
            self.timer.start()

    def _dispatch_registered_hotkey(self, hotkey_id: int) -> bool:
        if hotkey_id in (self.ID_F8, self.ID_VISIBILITY_CHORD):
            self._emit_action_edge("visibility", self.visibility_pressed)
            return True
        if hotkey_id in (self.ID_F9, self.ID_INTERACTION_CHORD):
            self._emit_action_edge("interaction", self.interaction_pressed)
            return True
        return False

    def _emit_action_edge(self, action: str, signal: Signal) -> None:
        if self._action_down[action]:
            return
        self._action_down[action] = True
        signal.emit()

    @staticmethod
    def _create_key_reader() -> Callable[[int], bool] | None:
        if sys.platform != "win32":
            return None
        get_async_key_state = ctypes.windll.user32.GetAsyncKeyState
        get_async_key_state.argtypes = [ctypes.c_int]
        get_async_key_state.restype = ctypes.c_short
        return lambda virtual_key: bool(get_async_key_state(virtual_key) & 0x8000)

    def _poll(self) -> None:
        if self._key_reader is None or not self._poll_enabled:
            return
        ctrl_alt = self._key_reader(self.VK_CONTROL) and self._key_reader(self.VK_ALT)
        states = (
            (
                "visibility",
                self._key_reader(self.VK_F8) or (ctrl_alt and self._key_reader(self.VK_O)),
                self.visibility_pressed,
            ),
            (
                "interaction",
                self._key_reader(self.interaction_virtual_key)
                or (ctrl_alt and self._key_reader(self.VK_I)),
                self.interaction_pressed,
            ),
        )
        for action, pressed, signal in states:
            if pressed:
                self._emit_action_edge(action, signal)
            else:
                self._action_down[action] = False
