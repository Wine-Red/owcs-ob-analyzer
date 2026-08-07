from __future__ import annotations

from dataclasses import dataclass
import math

import cv2
import numpy as np

from ..models import LifeMatch, LifeState


class LifeStateRecognizer:
    """Read blacked-out nameplates first, then structured health rails or X geometry."""

    @staticmethod
    def _name_black_score(image: np.ndarray | None) -> float | None:
        if image is None or image.size == 0:
            return None
        hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
        value = hsv[:, :, 2]
        if value.shape[0] > 8 and value.shape[1] > 8:
            value = value[3:-3, 3:-3]
        median_value = float(np.median(value))
        dark_ratio = float(np.mean(value < 65))
        median_score = float(np.clip((80.0 - median_value) / 45.0, 0.0, 1.0))
        coverage_score = float(np.clip((dark_ratio - 0.25) / 0.35, 0.0, 1.0))
        return median_score * 0.55 + coverage_score * 0.45

    @staticmethod
    def _health_rail_score(gray: np.ndarray) -> float:
        # Empty health cells remain outlined, so a real rail produces vertical
        # edges distributed across most of the slot even at very low health.
        vertical = np.abs(cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3))
        active = vertical[5:25].mean(axis=0) > 40
        columns = np.flatnonzero(active)
        if not len(columns):
            return 0.0
        span = float(columns[-1] - columns[0] + 1) / gray.shape[1]
        density = float(active.mean())
        span_score = float(np.clip((span - 0.45) / 0.45, 0.0, 1.0))
        density_score = float(np.clip((density - 0.08) / 0.22, 0.0, 1.0))
        return span_score * 0.70 + density_score * 0.30

    @staticmethod
    def _death_x_score(gray: np.ndarray) -> float:
        edges = cv2.Canny(gray, 20, 80)
        lines = cv2.HoughLinesP(
            edges,
            1,
            np.pi / 180,
            threshold=5,
            minLineLength=5,
            maxLineGap=5,
        )
        rising = 0
        falling = 0
        longest = 0.0
        if lines is not None:
            for x1, y1, x2, y2 in lines[:, 0]:
                centre_x = (x1 + x2) / 2.0
                length = math.hypot(x2 - x1, y2 - y1)
                if abs(centre_x - 64) > 40 or length < 7:
                    continue
                angle = math.degrees(math.atan2(y2 - y1, x2 - x1))
                if 30 <= angle <= 70:
                    rising += 1
                    longest = max(longest, length)
                elif -70 <= angle <= -30:
                    falling += 1
                    longest = max(longest, length)
        if not rising or not falling:
            return 0.0
        pair_score = min(1.0, min(rising, falling) / 2.0)
        length_score = float(np.clip((longest - 7.0) / 18.0, 0.0, 1.0))
        return pair_score * 0.70 + length_score * 0.30

    def match(
        self,
        image: np.ndarray,
        name_image: np.ndarray | None = None,
    ) -> LifeMatch:
        if image.size == 0:
            return LifeMatch("unknown", 0.0)
        sample = cv2.resize(image, (128, 32), interpolation=cv2.INTER_AREA)
        gray = cv2.cvtColor(sample, cv2.COLOR_BGR2GRAY)
        rail_score = self._health_rail_score(gray)
        name_black_score = self._name_black_score(name_image)

        # Blacked-out nameplates are invariant across observer team palettes
        # and remain reliable on bright maps where the world background can be
        # mistaken for a white health rail.
        if name_black_score is not None and name_black_score >= 0.62:
            return LifeMatch(
                "dead",
                min(0.99, 0.72 + name_black_score * 0.27),
                rail_score,
                name_black_score,
            )
        if rail_score >= 0.42:
            return LifeMatch(
                "alive",
                min(0.99, 0.62 + rail_score * 0.35),
                rail_score,
                0.0,
            )
        # Keep the X as a fallback for cropped evidence that lacks a nameplate.
        # A clearly non-black nameplate suppresses this background-sensitive
        # fallback; without either strong signal, return unknown.
        x_score = self._death_x_score(gray)
        if (name_black_score is None or name_black_score >= 0.25) and x_score >= 0.45:
            return LifeMatch(
                "dead",
                min(0.94, 0.58 + x_score * 0.34),
                rail_score,
                x_score,
            )
        death_score = max(x_score, name_black_score or 0.0)
        return LifeMatch(
            "unknown",
            max(rail_score, death_score) * 0.45,
            rail_score,
            death_score,
        )


@dataclass
class LifeStateStabilizer:
    value: LifeState = "unknown"
    confidence: float = 0.0
    candidate: LifeState = "unknown"
    candidate_count: int = 0

    def observe(self, match: LifeMatch) -> bool:
        if match.state == "unknown":
            self.candidate = "unknown"
            self.candidate_count = 0
            return False
        if match.state == self.value:
            self.confidence = max(self.confidence * 0.8, match.confidence)
            self.candidate = "unknown"
            self.candidate_count = 0
            return False
        if match.state == self.candidate:
            self.candidate_count += 1
        else:
            self.candidate = match.state
            self.candidate_count = 1
        # Death overlays can briefly resemble a live health rail. Preserve a
        # confirmed death until a strong live signal survives about one second
        # at the default 5 FPS; initial state and deaths remain responsive.
        if self.value == "dead" and match.state == "alive":
            if match.confidence < 0.68:
                self.candidate = "unknown"
                self.candidate_count = 0
                return False
            required = 5
        else:
            required = 2
        if self.candidate_count < required:
            return False
        self.value = match.state
        self.confidence = match.confidence
        self.candidate = "unknown"
        self.candidate_count = 0
        return True
