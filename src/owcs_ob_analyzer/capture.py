from __future__ import annotations

from dataclasses import dataclass
import ctypes
from pathlib import Path
import platform
import time
from typing import Protocol

import cv2
import numpy as np


@dataclass(frozen=True)
class CapturedFrame:
    image: np.ndarray
    full_size: tuple[int, int]
    captured_at: float
    source_name: str
    desktop_bounds: tuple[int, int, int, int] | None = None


class FrameSource(Protocol):
    def grab(self) -> CapturedFrame | None: ...

    def close(self) -> None: ...


def logical_hud_height(width: int, physical_height: int, aspect_ratio: float = 16 / 9) -> int:
    """Height used by a top-anchored logical HUD inside a borderless client."""
    return min(physical_height, round(width / aspect_ratio))


def overwatch_client_bounds() -> tuple[int, int, int, int] | None:
    """Return the visible Overwatch client rectangle in desktop coordinates."""
    if platform.system() != "Windows":
        return None
    user32 = ctypes.windll.user32
    matches: list[tuple[int, int, int, int]] = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    class Rect(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    class Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    def visit(hwnd: int, _: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buffer = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buffer, len(buffer))
        title = buffer.value.casefold()
        if "overwatch" not in title and "守望先锋" not in buffer.value:
            return True
        client = Rect()
        origin = Point(0, 0)
        if not user32.GetClientRect(hwnd, ctypes.byref(client)):
            return True
        if not user32.ClientToScreen(hwnd, ctypes.byref(origin)):
            return True
        width = client.right - client.left
        height = client.bottom - client.top
        if width >= 960 and height >= 540:
            matches.append((origin.x, origin.y, origin.x + width, origin.y + height))
        return True

    user32.EnumWindows(callback_type(visit), 0)
    return max(matches, key=lambda box: (box[2] - box[0]) * (box[3] - box[1]), default=None)


class ImageFrameSource:
    def __init__(self, path: Path, top_ratio: float = 0.24) -> None:
        self.path = Path(path)
        image = cv2.imdecode(np.fromfile(self.path, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise RuntimeError(f"无法读取测试图片：{self.path}")
        height, width = image.shape[:2]
        expected_height = round(width * 9 / 16)
        if height >= expected_height * 0.8:
            self.full_size = (width, height)
            self.image = image[: max(2, round(height * top_ratio))]
        else:
            self.full_size = (width, expected_height)
            required = max(2, round(expected_height * top_ratio))
            if height < required:
                self.image = cv2.copyMakeBorder(
                    image, 0, required - height, 0, 0, cv2.BORDER_CONSTANT
                )
            else:
                self.image = image[:required]

    def grab(self) -> CapturedFrame:
        return CapturedFrame(
            self.image.copy(),
            self.full_size,
            time.monotonic(),
            f"图片 · {self.path.name}",
            None,
        )

    def close(self) -> None:
        return None


class DxcamFrameSource:
    def __init__(self, top_ratio: float = 0.24, output_index: int = 0) -> None:
        import dxcam

        self.camera = dxcam.create(output_idx=output_index, output_color="BGR")
        initial = self.camera.grab()
        if initial is None:
            raise RuntimeError("DXGI 暂时没有返回画面")
        height, width = initial.shape[:2]
        game = overwatch_client_bounds()
        if game is not None and 0 <= game[0] < game[2] <= width and 0 <= game[1] < game[3] <= height:
            left, top, right, bottom = game
            self.full_size = (right - left, bottom - top)
            self.desktop_bounds = game
            hud_height = logical_hud_height(*self.full_size)
            self.region = (left, top, right, top + max(2, round(hud_height * top_ratio)))
            self.initial = initial[self.region[1] : self.region[3], self.region[0] : self.region[2]].copy()
            self.label = "Overwatch 窗口 · DXGI"
        else:
            self.full_size = (width, height)
            self.desktop_bounds = (0, 0, width, height)
            hud_height = logical_hud_height(*self.full_size)
            self.region = (0, 0, width, max(2, round(hud_height * top_ratio)))
            self.initial = initial[: self.region[3]].copy()
            self.label = "主显示器 · DXGI"

    def grab(self) -> CapturedFrame | None:
        if self.initial is not None:
            image, self.initial = self.initial, None
        else:
            image = self.camera.grab(region=self.region)
        if image is None:
            return None
        return CapturedFrame(
            image,
            self.full_size,
            time.monotonic(),
            self.label,
            self.desktop_bounds,
        )

    def close(self) -> None:
        self.camera.release()


class MssFrameSource:
    def __init__(self, top_ratio: float = 0.24, monitor_index: int = 1) -> None:
        import mss

        self.session = mss.mss()
        monitors = self.session.monitors
        if monitor_index < 1 or monitor_index >= len(monitors):
            raise RuntimeError(f"显示器编号超出范围：{monitor_index}")
        monitor = monitors[monitor_index]
        game = overwatch_client_bounds()
        if game is not None:
            left, top, right, bottom = game
            capture_left, capture_top = left, top
            self.full_size = (right - left, bottom - top)
            self.desktop_bounds = game
            self.label = "Overwatch 窗口 · MSS"
        else:
            capture_left, capture_top = int(monitor["left"]), int(monitor["top"])
            self.full_size = (int(monitor["width"]), int(monitor["height"]))
            self.desktop_bounds = (
                capture_left,
                capture_top,
                capture_left + self.full_size[0],
                capture_top + self.full_size[1],
            )
            self.label = "主显示器 · MSS"
        self.region = {
            "left": capture_left,
            "top": capture_top,
            "width": self.full_size[0],
            "height": max(2, round(logical_hud_height(*self.full_size) * top_ratio)),
        }

    def grab(self) -> CapturedFrame:
        bgra = np.asarray(self.session.grab(self.region))
        return CapturedFrame(
            bgra[:, :, :3].copy(),
            self.full_size,
            time.monotonic(),
            self.label,
            self.desktop_bounds,
        )

    def close(self) -> None:
        self.session.close()


def create_screen_source(top_ratio: float = 0.24) -> FrameSource:
    failures: list[str] = []
    try:
        return DxcamFrameSource(top_ratio)
    except Exception as exc:
        failures.append(f"DXGI: {exc}")
    try:
        return MssFrameSource(top_ratio)
    except Exception as exc:
        failures.append(f"MSS: {exc}")
    raise RuntimeError("无法初始化屏幕采集；" + "；".join(failures))
