from __future__ import annotations

import cv2
import numpy as np

from owcs_ob_analyzer.models import LifeMatch
from owcs_ob_analyzer.recognition.status import LifeStateRecognizer, LifeStateStabilizer


def _alive_rail() -> np.ndarray:
    image = np.full((32, 128, 3), 18, dtype=np.uint8)
    for left in range(18, 110, 11):
        cv2.rectangle(image, (left, 10), (left + 7, 20), (235, 235, 235), -1)
    return image


def _death_x(colour: tuple[int, int, int]) -> np.ndarray:
    image = np.full((32, 128, 3), 18, dtype=np.uint8)
    cv2.line(image, (54, 6), (74, 26), colour, 5)
    cv2.line(image, (74, 6), (54, 26), colour, 5)
    return image


def _name_plate(dead: bool) -> np.ndarray:
    colour = (34, 34, 34) if dead else (150, 105, 45)
    image = np.full((30, 128, 3), colour, dtype=np.uint8)
    cv2.putText(image, "PLAYER", (22, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (240, 240, 240), 1)
    return image


def test_alive_rail_is_detected() -> None:
    assert LifeStateRecognizer().match(_alive_rail()).state == "alive"


def test_death_x_is_colour_independent() -> None:
    recognizer = LifeStateRecognizer()
    for colour in ((25, 25, 235), (235, 120, 20), (35, 220, 80), (215, 215, 215)):
        assert recognizer.match(_death_x(colour)).state == "dead"


def test_black_nameplate_overrides_bright_map_background() -> None:
    bright_background = np.full((32, 128, 3), 228, dtype=np.uint8)
    cv2.line(bright_background, (48, 5), (78, 27), (40, 80, 235), 6)
    cv2.line(bright_background, (78, 5), (48, 27), (40, 80, 235), 6)
    match = LifeStateRecognizer().match(bright_background, _name_plate(dead=True))
    assert match.state == "dead"
    assert match.death_score >= 0.9


def test_bright_background_without_health_rail_is_not_alive() -> None:
    bright_background = np.full((32, 128, 3), 228, dtype=np.uint8)
    match = LifeStateRecognizer().match(bright_background, _name_plate(dead=False))
    assert match.state == "unknown"


def test_coloured_nameplate_and_structured_health_rail_are_alive() -> None:
    match = LifeStateRecognizer().match(_alive_rail(), _name_plate(dead=False))
    assert match.state == "alive"
    assert match.alive_score >= 0.42


def test_blank_status_stays_unknown() -> None:
    image = np.full((32, 128, 3), 18, dtype=np.uint8)
    assert LifeStateRecognizer().match(image).state == "unknown"


def test_life_state_requires_two_observations() -> None:
    recognizer = LifeStateRecognizer()
    state = LifeStateStabilizer()
    dead = recognizer.match(_death_x((30, 200, 90)))
    assert not state.observe(dead)
    assert state.value == "unknown"
    assert state.observe(dead)
    assert state.value == "dead"


def test_short_false_respawn_burst_does_not_flicker_dead_state() -> None:
    state = LifeStateStabilizer(value="dead", confidence=0.95)
    false_alive = LifeMatch("alive", 0.95, alive_score=1.0)
    for _ in range(4):
        assert not state.observe(false_alive)
        assert state.value == "dead"
    assert not state.observe(LifeMatch("dead", 0.9, death_score=0.9))
    assert state.value == "dead"
    assert state.candidate == "unknown"


def test_respawn_requires_five_strong_consecutive_observations() -> None:
    state = LifeStateStabilizer(value="dead", confidence=0.95)
    alive = LifeMatch("alive", 0.9, alive_score=0.9)
    for _ in range(4):
        assert not state.observe(alive)
    assert state.observe(alive)
    assert state.value == "alive"


def test_weak_alive_signal_cannot_resurrect_dead_player() -> None:
    state = LifeStateStabilizer(value="dead", confidence=0.95)
    weak_alive = LifeMatch("alive", 0.62, alive_score=0.2)
    for _ in range(8):
        assert not state.observe(weak_alive)
    assert state.value == "dead"
