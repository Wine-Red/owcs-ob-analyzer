from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

import cv2
import numpy as np

from ..capture import logical_hud_height
from ..config import HudLayout, crop_from_top
from .hero import HeroRecognizer

HudLocationStatus = Literal["searching", "locked"]


@dataclass(frozen=True)
class HudLocation:
    status: HudLocationStatus
    offset_y_px: int | None
    confidence: float


@dataclass(frozen=True)
class _Candidate:
    offset_y_px: int
    score: float
    support: int


class HudVerticalLocator:
    """Locate the spectator player strip without changing its crop geometry.

    Only a shared vertical translation is estimated. Team colours and player
    names are excluded from the structural shortlist. Official hero templates
    then verify crop geometry, but candidate identities never leave this gate.
    """

    def __init__(
        self,
        layout: HudLayout,
        hero_recognizer: HeroRecognizer,
        *,
        min_offset_ratio: float = -0.01,
        max_offset_ratio: float = 0.085,
        confirmations: int = 3,
        unlock_failures: int = 8,
        lock_tolerance_px: int = 2,
    ) -> None:
        self.layout = layout
        self.hero_recognizer = hero_recognizer
        self.min_offset_ratio = min_offset_ratio
        self.max_offset_ratio = max_offset_ratio
        self.confirmations = max(1, confirmations)
        self.unlock_failures = max(1, unlock_failures)
        self.lock_tolerance_px = max(0, lock_tolerance_px)
        self.locked_offset_y_px: int | None = None
        self._candidate_offset_y_px: int | None = None
        self._candidate_count = 0
        self._failed_verifications = 0
        self._confidence = 0.0

    def observe(
        self,
        top_frame: np.ndarray,
        full_size: tuple[int, int],
    ) -> HudLocation:
        if self.locked_offset_y_px is not None:
            candidate = self._probe_offset(
                top_frame,
                full_size,
                self.locked_offset_y_px,
            )
            if candidate is not None:
                self._failed_verifications = 0
                self._confidence = max(self._confidence * 0.8, candidate.score)
                return HudLocation(
                    "locked",
                    self.locked_offset_y_px,
                    min(0.99, self._confidence),
                )
            self._failed_verifications += 1
            # A failed geometry check must never feed a questionable crop into
            # the classifiers. Keep the internal lock briefly for recovery,
            # but report searching immediately so the analyzer fails closed.
            if self._failed_verifications < self.unlock_failures:
                return HudLocation("searching", None, 0.0)
            self.locked_offset_y_px = None
            self._candidate_offset_y_px = None
            self._candidate_count = 0
            self._failed_verifications = 0
            self._confidence = 0.0

        if self._candidate_offset_y_px is not None:
            candidate = self._probe_offset(
                top_frame,
                full_size,
                self._candidate_offset_y_px,
            )
        else:
            candidate = self.locate_once(top_frame, full_size)
        if candidate is None:
            self._candidate_offset_y_px = None
            self._candidate_count = 0
            return HudLocation("searching", None, 0.0)

        if (
            self._candidate_offset_y_px is not None
            and abs(candidate.offset_y_px - self._candidate_offset_y_px)
            <= self.lock_tolerance_px
        ):
            self._candidate_count += 1
            self._candidate_offset_y_px = round(
                (self._candidate_offset_y_px + candidate.offset_y_px) / 2
            )
        else:
            self._candidate_offset_y_px = candidate.offset_y_px
            self._candidate_count = 1
        self._confidence = candidate.score
        if self._candidate_count < self.confirmations:
            return HudLocation("searching", None, min(0.99, candidate.score))

        self.locked_offset_y_px = self._candidate_offset_y_px
        self._failed_verifications = 0
        return HudLocation(
            "locked",
            self.locked_offset_y_px,
            min(0.99, self._confidence),
        )

    def _probe_offset(
        self,
        top_frame: np.ndarray,
        full_size: tuple[int, int],
        offset_y_px: int,
    ) -> _Candidate | None:
        slots = self.layout.slots()
        matches = [
            self.hero_recognizer.match(
                crop_from_top(
                    top_frame,
                    full_size,
                    slots[index][2],
                    self.layout.logical_aspect_ratio,
                    offset_y_px,
                )
            )
            for index in (0, 2, 6, 9)
        ]
        accepted = sum(match.hero is not None for match in matches)
        average = float(np.mean([match.confidence for match in matches]))
        if accepted < 3 or average < 0.50:
            return None
        score = accepted / len(matches) * 0.72 + average * 0.28
        return _Candidate(offset_y_px, score, accepted)

    def locate_once(
        self,
        top_frame: np.ndarray,
        full_size: tuple[int, int],
    ) -> _Candidate | None:
        if top_frame.size == 0:
            return None
        width, physical_height = full_size
        logical_height = logical_hud_height(
            width,
            physical_height,
            self.layout.logical_aspect_ratio,
        )
        gray = cv2.cvtColor(top_frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.GaussianBlur(gray, (3, 3), 0)
        horizontal_edges = np.abs(cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3))
        vertical_edges = np.abs(cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3))

        min_offset = round(self.min_offset_ratio * logical_height)
        max_offset = round(self.max_offset_ratio * logical_height)
        max_base_y = max(
            box[3]
            for boxes in self.layout.status_rois.values()
            for box in boxes
        )
        max_visible_offset = top_frame.shape[0] - round(max_base_y * logical_height) - 1
        max_offset = min(max_offset, max_visible_offset)
        if max_offset < min_offset:
            return None

        structural_candidates = [
            self._score_offset(
                horizontal_edges,
                vertical_edges,
                width,
                logical_height,
                offset,
            )
            for offset in range(min_offset, max_offset + 1)
        ]
        peaks: list[_Candidate] = []
        for row in sorted(structural_candidates, key=lambda item: item.score, reverse=True):
            if row.support < 6 or row.score < 0.38:
                continue
            if all(abs(row.offset_y_px - peak.offset_y_px) >= 5 for peak in peaks):
                peaks.append(row)
            if len(peaks) >= 12:
                break
        if not peaks:
            return None

        offsets = sorted(
            {
                peak.offset_y_px + delta
                for peak in peaks
                for delta in (-2, -1, 0, 1, 2)
                if min_offset <= peak.offset_y_px + delta <= max_offset
            }
        )
        verified = self._verify_offsets(top_frame, full_size, offsets)
        if not verified:
            return None
        best = verified[0]
        runners = [
            row for row in verified[1:] if abs(row.offset_y_px - best.offset_y_px) >= 3
        ]
        runner_score = runners[0].score if runners else 0.0
        if best.support < 6 or best.score < 0.62 or best.score - runner_score < 0.08:
            return None
        return best

    def _verify_offsets(
        self,
        top_frame: np.ndarray,
        full_size: tuple[int, int],
        offsets: list[int],
    ) -> list[_Candidate]:
        slots = self.layout.slots()
        probe_indices = (0, 2, 6, 9)
        probe_results: list[tuple[float, int, int]] = []
        for offset in offsets:
            matches = [
                self.hero_recognizer.match(
                    crop_from_top(
                        top_frame,
                        full_size,
                        slots[index][2],
                        self.layout.logical_aspect_ratio,
                        offset,
                    )
                )
                for index in probe_indices
            ]
            accepted = sum(match.hero is not None for match in matches)
            average = float(np.mean([match.confidence for match in matches]))
            score = accepted / len(matches) * 0.72 + average * 0.28
            if accepted >= 2 and average >= 0.42:
                probe_results.append((score, accepted, offset))

        finalists: list[int] = []
        for _, _, offset in sorted(probe_results, reverse=True):
            if all(abs(offset - selected) >= 3 for selected in finalists):
                finalists.append(offset)
            if len(finalists) >= 4:
                break

        verified: list[_Candidate] = []
        for offset in finalists:
            matches = [
                self.hero_recognizer.match(
                    crop_from_top(
                        top_frame,
                        full_size,
                        hero_box,
                        self.layout.logical_aspect_ratio,
                        offset,
                    )
                )
                for _, _, hero_box, _, _ in slots
            ]
            accepted = sum(match.hero is not None for match in matches)
            average = float(np.mean([match.confidence for match in matches]))
            score = accepted / len(matches) * 0.76 + average * 0.24
            verified.append(_Candidate(offset, score, accepted))
        return sorted(verified, key=lambda row: row.score, reverse=True)

    def _score_offset(
        self,
        horizontal_edges: np.ndarray,
        vertical_edges: np.ndarray,
        width: int,
        logical_height: int,
        offset_y_px: int,
    ) -> _Candidate:
        slot_scores: list[float] = []
        rail_scores: list[float] = []
        for side in ("left", "right"):
            for slot in range(5):
                hero = self.layout.hero_rois[side][slot]
                name = self.layout.player_name_rois[side][slot]
                status = self.layout.status_rois[side][slot]
                x1 = max(0, round(min(hero[0], name[0], status[0]) * width))
                x2 = min(
                    horizontal_edges.shape[1],
                    round(max(hero[2], name[2], status[2]) * width),
                )
                boundaries = (hero[1], hero[3], name[3], status[3])
                boundary_scores = [
                    self._horizontal_line_score(
                        horizontal_edges,
                        x1,
                        x2,
                        round(y * logical_height) + offset_y_px,
                    )
                    for y in boundaries
                ]
                line_score = float(np.mean(sorted(boundary_scores)[-3:]))
                rail_score = self._health_rail_score(
                    vertical_edges,
                    width,
                    logical_height,
                    status,
                    offset_y_px,
                )
                slot_scores.append(line_score * 0.72 + rail_score * 0.28)
                rail_scores.append(rail_score)

        strongest_slots = sorted(slot_scores)[-8:]
        strongest_rails = sorted(rail_scores)[-5:]
        structure_score = float(np.mean(strongest_slots))
        rail_score = float(np.mean(strongest_rails))
        total = structure_score * 0.72 + rail_score * 0.28
        support = sum(score >= 0.34 for score in slot_scores)
        return _Candidate(offset_y_px, total, support)

    @staticmethod
    def _horizontal_line_score(
        edges: np.ndarray,
        x1: int,
        x2: int,
        y: int,
    ) -> float:
        if x2 - x1 < 8 or y < 3 or y + 3 >= edges.shape[0]:
            return 0.0
        line = edges[y - 1 : y + 2, x1:x2]
        nearby = np.concatenate(
            (
                edges[y - 7 : y - 3, x1:x2].reshape(-1),
                edges[y + 4 : y + 8, x1:x2].reshape(-1),
            )
        )
        line_energy = float(np.mean(line))
        nearby_energy = float(np.mean(nearby))
        ratio_score = float(
            np.clip((line_energy / (nearby_energy + 12.0) - 0.9) / 1.8, 0.0, 1.0)
        )
        coverage = float(np.mean(np.max(line, axis=0) >= 90.0))
        return ratio_score * 0.55 + coverage * 0.45

    @staticmethod
    def _health_rail_score(
        edges: np.ndarray,
        width: int,
        logical_height: int,
        box: tuple[float, float, float, float],
        offset_y_px: int,
    ) -> float:
        x1 = max(0, round(box[0] * width))
        x2 = min(edges.shape[1], round(box[2] * width))
        y1 = max(0, round(box[1] * logical_height) + offset_y_px)
        y2 = min(edges.shape[0], round(box[3] * logical_height) + offset_y_px)
        if x2 - x1 < 12 or y2 - y1 < 5:
            return 0.0
        profile = np.mean(edges[y1:y2, x1:x2], axis=0)
        active = profile >= 42.0
        columns = np.flatnonzero(active)
        if len(columns) < 4:
            return 0.0
        span = float(columns[-1] - columns[0] + 1) / len(profile)
        density = float(np.mean(active))
        transitions = float(np.mean(active[1:] != active[:-1]))
        span_score = float(np.clip((span - 0.45) / 0.45, 0.0, 1.0))
        density_score = float(np.clip((density - 0.08) / 0.25, 0.0, 1.0))
        repeat_score = float(np.clip((transitions - 0.08) / 0.24, 0.0, 1.0))
        return span_score * 0.45 + density_score * 0.25 + repeat_score * 0.30
