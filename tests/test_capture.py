from __future__ import annotations

from pathlib import Path

from owcs_ob_analyzer.capture import ImageFrameSource, logical_hud_height


def test_top_only_reference_uses_a_virtual_16_by_9_canvas() -> None:
    source = ImageFrameSource(Path(__file__).parent / "data" / "reference_top.png")
    packet = source.grab()
    width, height = packet.full_size
    assert abs(width / height - 16 / 9) < 0.01
    assert packet.image.shape[0] == round(height * 0.24)


def test_borderless_sixteen_by_ten_client_keeps_sixteen_by_nine_hud_canvas() -> None:
    assert logical_hud_height(2560, 1600) == 1440
