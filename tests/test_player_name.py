from __future__ import annotations

from owcs_ob_analyzer.recognition.player_name import (
    PlayerNameMatch,
    PlayerNameStabilizer,
    RapidOcrPlayerNameRecognizer,
    normalize_player_id,
)


def test_player_id_normalization_supports_chinese_and_english() -> None:
    assert normalize_player_id("  ＷＩＮＥＲＥＤ \n") == "WINERED"
    assert normalize_player_id("【酷不酷想不想学】") == "酷不酷想不想学"


def test_dual_pass_must_agree_before_name_becomes_a_candidate() -> None:
    class FakeEngine:
        def __init__(self, values: list[tuple[str, float]]) -> None:
            self.values = iter(values)

        def __call__(self, _image: object, **_kwargs: object) -> object:
            text, confidence = next(self.values)
            return ([[text, confidence]], 0.01)

    import numpy as np

    image = np.zeros((30, 120, 3), dtype=np.uint8)
    accepted = RapidOcrPlayerNameRecognizer(
        FakeEngine([(" Player_01 ", 0.97), ("Player_01", 0.95)])
    ).recognize(image)
    rejected = RapidOcrPlayerNameRecognizer(
        FakeEngine([("Player_01", 0.98), ("Player_02", 0.98)])
    ).recognize(image)
    assert accepted == PlayerNameMatch("Player_01", 0.95)
    assert rejected.text is None


def test_name_requires_three_matches_and_survives_transient_ocr_noise() -> None:
    stabilizer = PlayerNameStabilizer()
    assert not stabilizer.observe(PlayerNameMatch("WINERED", 0.97))
    assert not stabilizer.observe(PlayerNameMatch("winered", 0.96))
    assert stabilizer.observe(PlayerNameMatch("WINERED", 0.98))
    assert stabilizer.value == "WINERED"
    assert stabilizer.status == "confirmed"

    assert not stabilizer.observe(PlayerNameMatch("W1NERED", 0.99))
    assert not stabilizer.observe(PlayerNameMatch(None, 0.0))
    assert not stabilizer.observe(PlayerNameMatch("WINERED", 0.97))
    assert stabilizer.value == "WINERED"


def test_confirmed_name_needs_five_exact_matches_to_change() -> None:
    stabilizer = PlayerNameStabilizer(value="旧名字", confidence=0.98)
    for _ in range(4):
        assert not stabilizer.observe(PlayerNameMatch("NewName", 0.98))
        assert stabilizer.value == "旧名字"
    assert stabilizer.observe(PlayerNameMatch("NewName", 0.98))
    assert stabilizer.value == "NewName"


def test_low_contrast_name_needs_five_matches_instead_of_being_guessed_early() -> None:
    stabilizer = PlayerNameStabilizer()
    for _ in range(4):
        assert not stabilizer.observe(PlayerNameMatch("支援2", 0.75))
        assert stabilizer.value is None
    assert stabilizer.observe(PlayerNameMatch("支援2", 0.75))
    assert stabilizer.value == "支援2"
