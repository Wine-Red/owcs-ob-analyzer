from __future__ import annotations

import numpy as np

from owcs_ob_analyzer.config import DEFAULT_LAYOUT, crop_from_top


def test_layout_has_five_slots_per_side_and_avoids_team_colour_contracts() -> None:
    assert len(DEFAULT_LAYOUT.hero_rois["left"]) == 5
    assert len(DEFAULT_LAYOUT.hero_rois["right"]) == 5
    assert len(DEFAULT_LAYOUT.player_name_rois["left"]) == 5
    assert len(DEFAULT_LAYOUT.player_name_rois["right"]) == 5
    assert len(DEFAULT_LAYOUT.status_rois["left"]) == 5
    assert len(DEFAULT_LAYOUT.status_rois["right"]) == 5
    assert not hasattr(DEFAULT_LAYOUT, "team_colours")


def test_top_crop_uses_logical_hud_coordinates() -> None:
    top = np.zeros((230, 1920, 3), dtype=np.uint8)
    crop = crop_from_top(top, (1920, 1080), (0.1, 0.05, 0.2, 0.10))
    assert crop.shape == (54, 192, 3)


def test_vertical_offset_moves_crop_without_resizing_it() -> None:
    top = np.zeros((300, 1920, 3), dtype=np.uint8)
    box = (0.1, 0.05, 0.2, 0.10)
    baseline = crop_from_top(top, (1920, 1080), box)
    shifted = crop_from_top(top, (1920, 1080), box, vertical_offset_px=72)
    assert shifted.shape == baseline.shape


def test_sixteen_by_ten_borderless_client_uses_logical_sixteen_by_nine_hud_height() -> None:
    top = np.zeros((230, 2560, 3), dtype=np.uint8)
    crop = crop_from_top(top, (2560, 1600), (0.047, 0.0674, 0.074, 0.1029))
    assert crop.shape == (51, 69, 3)
