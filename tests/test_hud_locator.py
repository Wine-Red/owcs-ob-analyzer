from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np
import pytest

from owcs_ob_analyzer.analyzer import RealtimeAnalyzer, resource_root
from owcs_ob_analyzer.config import DEFAULT_LAYOUT
from owcs_ob_analyzer.recognition import HeroRecognizer, HudVerticalLocator


REFERENCE = Path(__file__).parent / "data" / "reference_top.png"
EXPECTED_HEROES = [
    "ana",
    "sierra",
    "roadhog",
    "wuyang",
    "genji",
    "anran",
    "freja",
    "moira",
    "mizuki",
    "wrecking-ball",
]
EXPECTED_LIFE_STATES = [
    "dead",
    "alive",
    "alive",
    "alive",
    "dead",
    "dead",
    "alive",
    "alive",
    "alive",
    "dead",
]


def _reference() -> np.ndarray:
    return cv2.imdecode(np.fromfile(REFERENCE, dtype=np.uint8), cv2.IMREAD_COLOR)


def _shift_down(image: np.ndarray, offset_y_px: int) -> np.ndarray:
    shifted = np.zeros_like(image)
    shifted[offset_y_px:] = image[:-offset_y_px]
    return shifted


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_locator_requires_three_consistent_frames_before_locking() -> None:
    frame = _reference()
    recognizer = HeroRecognizer(resource_root() / "heroes" / "illustrated")
    locator = HudVerticalLocator(DEFAULT_LAYOUT, recognizer)

    assert locator.observe(frame, (2559, 1439)).status == "searching"
    assert locator.observe(frame, (2559, 1439)).status == "searching"
    locked = locator.observe(frame, (2559, 1439))
    assert locked.status == "locked"
    assert locked.offset_y_px is not None
    assert abs(locked.offset_y_px) <= 1
    assert locked.confidence >= 0.8


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
@pytest.mark.parametrize("offset_y_px", [45, 72])
def test_locator_recovers_only_the_vertical_translation(offset_y_px: int) -> None:
    frame = _shift_down(_reference(), offset_y_px)
    recognizer = HeroRecognizer(resource_root() / "heroes" / "illustrated")
    locator = HudVerticalLocator(DEFAULT_LAYOUT, recognizer)
    location = locator.locate_once(frame, (2559, 1600))
    assert location is not None
    assert location.offset_y_px == offset_y_px
    assert location.support == 10


def test_blank_frame_never_locks() -> None:
    frame = np.full((345, 2559, 3), 80, dtype=np.uint8)
    recognizer = HeroRecognizer(resource_root() / "heroes" / "illustrated")
    locator = HudVerticalLocator(DEFAULT_LAYOUT, recognizer)
    for _ in range(5):
        location = locator.observe(frame, (2559, 1439))
        assert location.status == "searching"
        assert location.offset_y_px is None


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_horizontal_misalignment_is_rejected_instead_of_corrected() -> None:
    reference = _reference()
    shifted = np.zeros_like(reference)
    shifted[:, 15:] = reference[:, :-15]
    recognizer = HeroRecognizer(resource_root() / "heroes" / "illustrated")
    locator = HudVerticalLocator(DEFAULT_LAYOUT, recognizer)
    assert locator.locate_once(shifted, (2559, 1439)) is None


def test_visual_noise_cannot_form_a_false_hud_lock() -> None:
    frame = np.random.default_rng(42).integers(
        0,
        256,
        size=(345, 2559, 3),
        dtype=np.uint8,
    )
    recognizer = HeroRecognizer(resource_root() / "heroes" / "illustrated")
    locator = HudVerticalLocator(DEFAULT_LAYOUT, recognizer)
    assert locator.locate_once(frame, (2559, 1439)) is None


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_shifted_hud_keeps_the_same_confirmed_hero_results() -> None:
    frame = _shift_down(_reference(), 72)
    analyzer = RealtimeAnalyzer(max_hero_jobs=2, enable_player_name_ocr=False)
    snapshot = None
    for index in range(25):
        snapshot = analyzer.analyze(
            frame,
            (2559, 1600),
            captured_at=float(index),
            source_name="shifted 16:9 HUD in 16:10 client",
        )
    assert snapshot is not None
    assert snapshot.hud_status == "locked"
    assert snapshot.hud_offset_y_px == 72
    assert [row.hero for row in snapshot.slots] == EXPECTED_HEROES
    assert [row.life_state for row in snapshot.slots] == EXPECTED_LIFE_STATES


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_lost_hud_immediately_stops_classification() -> None:
    frame = _reference()
    analyzer = RealtimeAnalyzer(max_hero_jobs=10, enable_player_name_ocr=False)
    for index in range(3):
        snapshot = analyzer.analyze(frame, (2559, 1439), captured_at=float(index))
    assert snapshot.hud_status == "locked"

    blank = np.full_like(frame, 80)
    lost = analyzer.analyze(blank, (2559, 1439), captured_at=4.0)
    assert lost.hud_status == "searching"
    assert lost.hud_offset_y_px is None
    assert lost.classified_slots == 0
