from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from ..models import HeroMatch


def _fingerprint(image: np.ndarray) -> str:
    gray = cv2.resize(cv2.cvtColor(image, cv2.COLOR_BGR2GRAY), (9, 8))
    bits = gray[:, 1:] > gray[:, :-1]
    return np.packbits(bits.flatten()).tobytes().hex()


def _hamming(left: str, right: str) -> int:
    return sum(
        (a ^ b).bit_count()
        for a, b in zip(bytes.fromhex(left), bytes.fromhex(right))
    )


class HeroRecognizer:
    """Fail-closed matcher for the official illustrated HUD portraits.

    Spectator portraits are deterministic crops of the official square art.
    We therefore render the small set of possible HUD crops once, compare
    colour-independent edge magnitude, and only publish a hero when both the
    absolute similarity and the lead over the runner-up are strong.
    """

    canonical_size = (64, 48)  # width, height
    min_similarity = 0.50
    min_margin = 0.20
    _geometry = (
        (1.84, 1.20, 0.37),
        (1.78, 1.16, 0.33),
        (1.78, 1.16, 0.41),
        (1.78, 1.24, 0.33),
        (1.78, 1.24, 0.41),
        (1.90, 1.16, 0.33),
        (1.90, 1.16, 0.41),
        (1.90, 1.24, 0.33),
        (1.90, 1.24, 0.41),
    )

    def __init__(self, icon_dir: Path, model_path: Path | None = None) -> None:
        self.icon_dir = Path(icon_dir)
        # Retain this argument for callers built against the first release. The
        # learned model is deliberately excluded from formal output because it
        # can confidently confuse visually similar heroes.
        del model_path
        heroes: list[str] = []
        template_rows: list[np.ndarray] = []
        template_hero_indices: list[int] = []
        for path in sorted(self.icon_dir.glob("*.png")):
            image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
            if image is None:
                continue
            image = self._composite_alpha(image)
            hero_index = len(heroes)
            heroes.append(path.stem.lower())
            for scale, x_offset, y_offset in self._geometry:
                for mirrored in (False, True):
                    rendered = self._render_hud_crop(
                        image,
                        scale,
                        x_offset,
                        y_offset,
                        mirrored,
                    )
                    template_rows.append(self._features(rendered, resize=False))
                    template_hero_indices.append(hero_index)
        if not heroes:
            raise RuntimeError(f"未找到可读取的英雄头像：{self.icon_dir}")
        self.heroes = heroes
        self.templates = np.ascontiguousarray(template_rows, dtype=np.float32)
        self.template_hero_indices = np.asarray(template_hero_indices, dtype=np.int32)

    @staticmethod
    def _composite_alpha(image: np.ndarray) -> np.ndarray:
        if image.ndim == 3 and image.shape[2] == 4:
            alpha = image[:, :, 3:4].astype(np.float32) / 255.0
            return np.asarray(
                image[:, :, :3] * alpha + 127.0 * (1.0 - alpha),
                dtype=np.uint8,
            )
        return image[:, :, :3]

    @classmethod
    def _render_hud_crop(
        cls,
        image: np.ndarray,
        scale_factor: float,
        x_factor: float,
        y_factor: float,
        mirrored: bool,
    ) -> np.ndarray:
        width, height = cls.canonical_size
        source = cv2.flip(image, 1) if mirrored else image
        size = max(width, height, round(height * scale_factor))
        scaled = cv2.resize(source, (size, size), interpolation=cv2.INTER_AREA)
        left = max(0, round(width - height * x_factor))
        top = max(0, round(height * y_factor))
        return scaled[top : top + height, left : left + width]

    @classmethod
    def _features(cls, image: np.ndarray, *, resize: bool = True) -> np.ndarray:
        if image.size == 0:
            return np.zeros(
                cls.canonical_size[0] * cls.canonical_size[1],
                dtype=np.float32,
            )
        if resize:
            width, height = cls.canonical_size
            interpolation = cv2.INTER_AREA if image.shape[1] > width else cv2.INTER_CUBIC
            image = cv2.resize(image, (width, height), interpolation=interpolation)
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        gray = cv2.GaussianBlur(gray, (3, 3), 0)
        dx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)
        dy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)
        vector = cv2.magnitude(dx, dy).reshape(-1)
        vector -= float(vector.mean())
        norm = float(np.linalg.norm(vector))
        if norm <= 1e-6:
            return np.zeros_like(vector)
        return vector / norm

    def match(self, image: np.ndarray) -> HeroMatch:
        if image.size == 0:
            return HeroMatch(None, 0.0, [])
        feature = self._features(image)
        if not np.any(feature):
            return HeroMatch(None, 0.0, [])
        template_scores = self.templates @ feature
        hero_scores = np.full(len(self.heroes), -1.0, dtype=np.float32)
        np.maximum.at(hero_scores, self.template_hero_indices, template_scores)
        ranking = sorted(
            zip(self.heroes, (float(score) for score in hero_scores)),
            key=lambda row: row[1],
            reverse=True,
        )
        best = ranking[0]
        runner_score = ranking[1][1] if len(ranking) > 1 else -1.0
        margin = best[1] - runner_score
        accepted = best[1] >= self.min_similarity and margin >= self.min_margin
        confidence = float(np.clip(best[1], 0.0, 0.99))
        return HeroMatch(best[0] if accepted else None, confidence, ranking[:5])


@dataclass
class PortraitChangeGate:
    threshold: int = 5
    # Periodic audits correct a rare bad initial lock without continuously
    # running all ten portrait matchers. Switch hysteresis absorbs audit noise.
    refresh_s: float = 6.0
    fingerprint: str | None = None
    last_classified_s: float = -1e9

    def should_classify(self, image: np.ndarray, now: float, pending: bool = False) -> bool:
        if pending or self.fingerprint is None or now - self.last_classified_s >= self.refresh_s:
            return True
        return _hamming(_fingerprint(image), self.fingerprint) > self.threshold

    def commit(self, image: np.ndarray, now: float) -> None:
        self.fingerprint = _fingerprint(image)
        self.last_classified_s = now


@dataclass
class HeroStabilizer:
    value: str | None = None
    confidence: float = 0.0
    candidate: str | None = None
    candidate_confidence: float = 0.0
    candidate_count: int = 0

    @property
    def pending(self) -> bool:
        return self.candidate is not None

    def _clear_candidate(self) -> None:
        self.candidate = None
        self.candidate_confidence = 0.0
        self.candidate_count = 0

    def observe(self, match: HeroMatch) -> bool:
        hero = match.hero
        minimum_confidence = 0.22 if self.value is None else 0.30
        if hero is None or match.confidence < minimum_confidence:
            # Confirmation must be continuous. Sparse matching noise must not
            # accumulate into a future hero switch.
            self._clear_candidate()
            return False
        if hero == self.value:
            self.confidence = max(self.confidence * 0.75, match.confidence)
            self._clear_candidate()
            return False
        if hero == self.candidate:
            self.candidate_count += 1
            self.candidate_confidence += (
                match.confidence - self.candidate_confidence
            ) / self.candidate_count
        else:
            self.candidate = hero
            self.candidate_count = 1
            self.candidate_confidence = match.confidence
        if self.value is None:
            required = 3 if self.candidate_confidence >= 0.62 else 4
        else:
            required = 5 if self.candidate_confidence >= 0.62 else 7
        if self.candidate_count < required:
            return False
        self.value = hero
        self.confidence = self.candidate_confidence
        self._clear_candidate()
        return True
