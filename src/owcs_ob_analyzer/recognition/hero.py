from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

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


class HeroPortraitClassifier:
    """Tiny colour-invariant HOG prototype model used as supporting evidence."""

    def __init__(self, model_path: Path) -> None:
        payload = np.load(model_path, allow_pickle=False)
        self.heroes = [str(value) for value in payload["heroes"].tolist()]
        self.prototypes = np.asarray(payload["prototypes"], dtype=np.float32)
        self.prototype_heroes = np.asarray(payload["prototype_heroes"], dtype=np.int32)
        self.reviewed_heroes = set(
            str(value)
            for value in (
                payload["reviewed_heroes"].tolist()
                if "reviewed_heroes" in payload.files
                else self.heroes
            )
        )
        self.min_similarity = float(np.asarray(payload["min_similarity"]).reshape(-1)[0])
        self.min_margin = float(np.asarray(payload["min_margin"]).reshape(-1)[0])
        self.hog = cv2.HOGDescriptor((48, 48), (16, 16), (8, 8), (8, 8), 9)

    def _features(self, image: np.ndarray) -> np.ndarray:
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        gray = cv2.resize(gray, (48, 48), interpolation=cv2.INTER_AREA)
        equalized = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(4, 4)).apply(gray)
        vector = self.hog.compute(equalized).reshape(-1).astype(np.float32)
        return vector / max(float(np.linalg.norm(vector)), 1e-6)

    def predict(self, images: Sequence[np.ndarray]) -> HeroMatch:
        features = [self._features(image) for image in images if image.size]
        if not features:
            return HeroMatch(None, 0.0, [])
        rows: list[tuple[str, float]] = []
        for hero_index, hero in enumerate(self.heroes):
            indices = np.flatnonzero(self.prototype_heroes == hero_index)
            if not len(indices):
                continue
            per_view = [float(np.max(self.prototypes[indices] @ feature)) for feature in features]
            rows.append((hero, float(np.median(per_view))))
        rows.sort(key=lambda row: row[1], reverse=True)
        if not rows:
            return HeroMatch(None, 0.0, [])
        best = rows[0]
        runner = rows[1][1] if len(rows) > 1 else -1.0
        margin = best[1] - runner
        accepted = best[1] >= self.min_similarity and margin >= self.min_margin
        confidence = float(np.clip(0.5 + (best[1] - 0.55) * 0.8 + margin * 2.0, 0.0, 0.99))
        hero = best[0] if accepted and best[0] in self.reviewed_heroes else None
        return HeroMatch(hero, confidence, rows[:5])


class HeroRecognizer:
    """SIFT portrait matcher resilient to scale, desaturation, and team tint."""

    min_visible_features = 55

    def __init__(self, icon_dir: Path, model_path: Path | None = None) -> None:
        self.icon_dir = Path(icon_dir)
        self.detector = cv2.SIFT_create(nfeatures=300)
        self.matcher = cv2.BFMatcher(cv2.NORM_L2)
        self.references: dict[str, list[np.ndarray]] = {}
        for path in sorted(self.icon_dir.glob("*.png")):
            image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
            if image is None:
                continue
            image = cv2.resize(image, (128, 128), interpolation=cv2.INTER_AREA)
            variants: list[np.ndarray] = []
            for variant in (image, cv2.flip(image, 1)):
                _, descriptors = self.detector.detectAndCompute(variant, None)
                if descriptors is not None:
                    variants.append(descriptors)
            if variants:
                self.references[path.stem.lower()] = variants
        if not self.references:
            raise RuntimeError(f"未找到可读取的英雄头像：{self.icon_dir}")
        self.learned = None
        if model_path is not None and Path(model_path).is_file():
            try:
                self.learned = HeroPortraitClassifier(Path(model_path))
            except (OSError, ValueError, KeyError):
                self.learned = None

    @staticmethod
    def _views(image: np.ndarray) -> list[np.ndarray]:
        if image.size == 0:
            return []
        height, width = image.shape[:2]
        views = [cv2.resize(image, (128, 128), interpolation=cv2.INTER_CUBIC)]
        if width != height:
            size = min(width, height)
            left = max(0, (width - size) // 2)
            top = max(0, (height - size) // 2)
            views.append(cv2.resize(image[top : top + size, left : left + size], (128, 128)))
        return views

    def match(self, image: np.ndarray) -> HeroMatch:
        descriptor_sets: list[np.ndarray] = []
        views = self._views(image)
        for sample in views:
            _, descriptors = self.detector.detectAndCompute(sample, None)
            if descriptors is not None and len(descriptors) >= self.min_visible_features:
                descriptor_sets.append(descriptors)
        # Keep the learned model on the original HUD crop. Square SIFT views
        # are useful for local features, but duplicating them would overweight
        # the centre crop in the prototype vote.
        learned = self.learned.predict([image]) if self.learned is not None else None
        if not descriptor_sets:
            return HeroMatch(None, 0.0, learned.candidates if learned else [])

        scores: list[tuple[str, float]] = []
        for hero, references in self.references.items():
            variants: list[float] = []
            for descriptors in descriptor_sets:
                for reference in references:
                    pairs = self.matcher.knnMatch(descriptors, reference, k=2)
                    good = [first for first, second in pairs if first.distance < 0.75 * second.distance]
                    variants.append(len(good) / max(10.0, min(len(descriptors), len(reference)) * 0.35))
            scores.append((hero, min(1.0, max(variants, default=0.0))))
        scores.sort(key=lambda row: row[1], reverse=True)
        best = scores[0]
        runner = scores[1][1] if len(scores) > 1 else 0.0
        margin = max(0.0, best[1] - runner)
        confidence = min(0.99, best[1] * 0.75 + margin * 0.50)
        accepted = (
            confidence >= 0.38
            or (best[1] >= 0.32 and margin >= 0.05)
            or (best[1] >= 0.38 and margin >= 0.015)
        )
        if learned is not None and learned.hero == best[0]:
            confidence = min(0.99, max(confidence, learned.confidence * 0.92) + 0.04)
            accepted = accepted or learned.confidence >= 0.78
        elif (
            not accepted
            and learned is not None
            and learned.hero is not None
            and learned.confidence >= 0.92
        ):
            return learned
        return HeroMatch(best[0] if accepted else None, confidence, scores[:5])


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
