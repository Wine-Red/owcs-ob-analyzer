from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np
import pytest

from owcs_ob_analyzer.analyzer import RealtimeAnalyzer
from owcs_ob_analyzer.app import analyze_image


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
EXPECTED_PLAYER_NAMES = [
    "橘皮",
    "酷不酷想不想学",
    "云天衣",
    "诺诺诺利",
    "SYUNN",
    "樱琉寒绯",
    "六花不是花",
    "秋名山车神",
    "巨硕之斧",
    "WINERED",
]


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_supplied_reference_recognizes_all_heroes_and_life_states() -> None:
    result = analyze_image(REFERENCE)
    slots = result["slots"]
    assert [row["hero"] for row in slots] == EXPECTED_HEROES
    assert [row["life_state"] for row in slots] == EXPECTED_LIFE_STATES
    assert [row["player_name"] for row in slots] == EXPECTED_PLAYER_NAMES
    assert all(row["player_name_status"] == "confirmed" for row in slots)
    assert all(row["player_name_confidence"] >= 0.90 for row in slots)
    assert [event["kind"] for event in slots[0]["events"]] == ["death", "hero_pick"]
    assert all(any(event["kind"] == "hero_pick" for event in row["events"]) for row in slots)
    assert result["elapsed_s"] >= 0
    assert len(result["timeline"]) >= 14


@pytest.mark.skipif(not REFERENCE.is_file(), reason="reference screenshot not packaged")
def test_reference_uses_same_rois_inside_a_sixteen_by_ten_borderless_client() -> None:
    frame = cv2.imdecode(np.fromfile(REFERENCE, dtype=np.uint8), cv2.IMREAD_COLOR)
    analyzer = RealtimeAnalyzer(max_hero_jobs=2, enable_player_name_ocr=False)
    snapshot = None
    for index in range(22):
        snapshot = analyzer.analyze(
            frame[:230],
            (frame.shape[1], 1600),
            captured_at=float(index),
            source_name="16:10 borderless / 16:9 HUD",
        )
    assert snapshot is not None
    assert [row.hero for row in snapshot.slots] == EXPECTED_HEROES
    assert [row.life_state for row in snapshot.slots] == EXPECTED_LIFE_STATES
    assert all(row.player_name_image_png for row in snapshot.slots)
    first_name_strip = cv2.imdecode(
        np.frombuffer(snapshot.slots[0].player_name_image_png, dtype=np.uint8),
        cv2.IMREAD_COLOR,
    )
    assert first_name_strip.shape[0] == 30
    assert first_name_strip.shape[1] >= 125
