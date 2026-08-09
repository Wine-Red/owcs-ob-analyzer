from __future__ import annotations

import ctypes
from ctypes import wintypes
import sys

from PySide6.QtWidgets import QWidget


HWND_TOPMOST = -1
SW_RESTORE = 9
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040


def bring_to_front(window: QWidget) -> bool:
    """Show a topmost Qt tool window and give it foreground input on Windows."""
    window.show()
    window.raise_()
    window.activateWindow()
    if sys.platform != "win32":
        return True

    hwnd = wintypes.HWND(int(window.winId()))
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.c_void_p]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    user32.AttachThreadInput.restype = wintypes.BOOL
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.ShowWindow.restype = wintypes.BOOL
    user32.SetWindowPos.argtypes = [
        wintypes.HWND,
        wintypes.HWND,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.UINT,
    ]
    user32.SetWindowPos.restype = wintypes.BOOL
    user32.BringWindowToTop.argtypes = [wintypes.HWND]
    user32.BringWindowToTop.restype = wintypes.BOOL
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.SetForegroundWindow.restype = wintypes.BOOL
    user32.SetActiveWindow.argtypes = [wintypes.HWND]
    user32.SetActiveWindow.restype = wintypes.HWND
    kernel32.GetCurrentThreadId.restype = wintypes.DWORD
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetWindowPos(
        hwnd,
        wintypes.HWND(HWND_TOPMOST),
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
    )

    foreground = user32.GetForegroundWindow()
    foreground_thread = (
        user32.GetWindowThreadProcessId(foreground, None) if foreground else 0
    )
    current_thread = kernel32.GetCurrentThreadId()
    attached = bool(
        foreground_thread
        and foreground_thread != current_thread
        and user32.AttachThreadInput(current_thread, foreground_thread, True)
    )
    try:
        brought_to_top = bool(user32.BringWindowToTop(hwnd))
        activated = bool(user32.SetForegroundWindow(hwnd))
        user32.SetActiveWindow(hwnd)
        return activated or brought_to_top
    finally:
        if attached:
            user32.AttachThreadInput(current_thread, foreground_thread, False)
