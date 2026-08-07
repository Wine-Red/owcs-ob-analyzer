from __future__ import annotations

import numpy as np

from owcs_ob_analyzer.analyzer import RealtimeAnalyzer
from owcs_ob_analyzer.models import HeroMatch, LifeMatch


class _HeroRecognizer:
    hero = "ana"

    def match(self, _image: np.ndarray) -> HeroMatch:
        return HeroMatch(self.hero, 0.8, [(self.hero, 0.8)])


class _LifeRecognizer:
    state = "alive"

    def match(
        self,
        _image: np.ndarray,
        _name_image: np.ndarray | None = None,
    ) -> LifeMatch:
        return LifeMatch(self.state, 0.9)


def test_each_slot_keeps_its_own_switch_death_and_respawn_timeline() -> None:
    analyzer = RealtimeAnalyzer(max_hero_jobs=10)
    heroes = _HeroRecognizer()
    lives = _LifeRecognizer()
    analyzer.hero_recognizer = heroes
    analyzer.life_recognizer = lives
    frame = np.zeros((230, 1920, 3), dtype=np.uint8)

    analyzer.analyze(frame, (1920, 1080), captured_at=0.0)
    analyzer.analyze(frame, (1920, 1080), captured_at=1.0)
    snapshot = analyzer.analyze(frame, (1920, 1080), captured_at=2.0)
    assert [event.kind for event in snapshot.slots[0].events] == ["hero_pick"]

    heroes.hero = "kiriko"
    lives.state = "dead"
    for runtime in analyzer.slots:
        runtime.gate.fingerprint = None
    for captured_at in range(3, 7):
        analyzer.analyze(frame, (1920, 1080), captured_at=float(captured_at))
    snapshot = analyzer.analyze(frame, (1920, 1080), captured_at=7.0)
    assert [event.kind for event in snapshot.slots[0].events] == [
        "hero_pick",
        "death",
        "hero_switch",
    ]
    assert snapshot.slots[0].events[-1].previous_hero == "ana"
    assert snapshot.slots[0].events[-1].hero == "kiriko"

    lives.state = "alive"
    for captured_at in range(8, 12):
        analyzer.analyze(frame, (1920, 1080), captured_at=float(captured_at))
    snapshot = analyzer.analyze(frame, (1920, 1080), captured_at=12.0)
    assert snapshot.slots[0].events[-1].kind == "respawn"
    assert snapshot.slots[0].events[-1].elapsed_s == 12.0
