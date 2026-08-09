from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np

from owcs_ob_analyzer.models import HeroMatch
from owcs_ob_analyzer.recognition.hero import (
    HeroRecognizer,
    HeroStabilizer,
    PortraitChangeGate,
)


ROOT = Path(__file__).parents[1]
HERO_ICONS = ROOT / "src" / "owcs_ob_analyzer" / "resources" / "heroes" / "illustrated"
ANA_REGRESSION = Path(__file__).parent / "data" / "hero_ana_regression.png"


def _load_image(path: Path) -> np.ndarray:
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def test_official_template_matcher_fixes_ana_hazard_regression() -> None:
    recognizer = HeroRecognizer(HERO_ICONS)
    match = recognizer.match(_load_image(ANA_REGRESSION))
    scores = dict(match.candidates)

    assert match.hero == "ana"
    assert match.candidates[0][0] == "ana"
    assert scores["ana"] - scores.get("hazard", -1.0) >= recognizer.min_margin


def test_official_template_matcher_is_scale_independent() -> None:
    recognizer = HeroRecognizer(HERO_ICONS)
    portrait = _load_image(ANA_REGRESSION)
    for scale in (0.5, 1.0, 1.5):
        resized = cv2.resize(portrait, None, fx=scale, fy=scale)
        assert recognizer.match(resized).hero == "ana"


def test_official_template_matcher_rejects_featureless_input() -> None:
    recognizer = HeroRecognizer(HERO_ICONS)
    match = recognizer.match(np.full((48, 64, 3), 127, dtype=np.uint8))
    assert match.hero is None
    assert match.confidence == 0.0


def test_hero_switch_requires_temporal_confirmation() -> None:
    state = HeroStabilizer(value="ana", confidence=0.8)
    switch = HeroMatch("kiriko", 0.75, [("kiriko", 0.75)])
    for _ in range(4):
        assert not state.observe(switch)
        assert state.value == "ana"
        assert state.pending
    assert state.observe(switch)
    assert state.value == "kiriko"


def test_unknown_match_never_erases_confirmed_hero() -> None:
    state = HeroStabilizer(value="ana", confidence=0.8)
    assert not state.observe(HeroMatch(None, 0.0, []))
    assert state.value == "ana"


def test_low_confidence_switch_needs_seven_observations() -> None:
    state = HeroStabilizer(value="ana", confidence=0.8)
    switch = HeroMatch("kiriko", 0.4, [("kiriko", 0.4)])
    for _ in range(6):
        assert not state.observe(switch)
    assert state.observe(switch)


def test_initial_hero_needs_three_continuous_high_confidence_observations() -> None:
    state = HeroStabilizer()
    match = HeroMatch("ana", 0.8, [("ana", 0.8)])
    assert not state.observe(match)
    assert not state.observe(match)
    assert state.observe(match)
    assert state.value == "ana"


def test_unknown_frame_breaks_a_hero_switch_candidate() -> None:
    state = HeroStabilizer(value="ana", confidence=0.8)
    wrong = HeroMatch("kiriko", 0.8, [("kiriko", 0.8)])
    for _ in range(4):
        assert not state.observe(wrong)
    assert state.pending
    assert not state.observe(HeroMatch(None, 0.0, []))
    assert not state.pending
    assert state.value == "ana"
    assert not state.observe(wrong)


def test_short_wrong_hero_burst_never_replaces_confirmed_hero() -> None:
    state = HeroStabilizer(value="ana", confidence=0.8)
    wrong = HeroMatch("kiriko", 0.9, [("kiriko", 0.9)])
    correct = HeroMatch("ana", 0.7, [("ana", 0.7)])
    for _ in range(3):
        assert not state.observe(wrong)
    assert not state.observe(correct)
    assert state.value == "ana"
    assert not state.pending


def test_stable_portrait_is_periodically_reaudited_after_six_seconds() -> None:
    portrait = np.zeros((40, 50, 3), dtype=np.uint8)
    gate = PortraitChangeGate()
    gate.commit(portrait, 0.0)
    assert not gate.should_classify(portrait, 5.9)
    assert gate.should_classify(portrait, 6.0)
