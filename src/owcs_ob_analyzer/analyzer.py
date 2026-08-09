from __future__ import annotations

from dataclasses import dataclass, field
from collections import deque
from pathlib import Path
import time

import cv2
import numpy as np

from .config import DEFAULT_LAYOUT, HudLayout, crop_from_top
from .models import AnalysisSnapshot, PlayerEvent, SlotSnapshot, TimelineEvent
from .recognition import (
    HeroRecognizer,
    HeroStabilizer,
    HudLocation,
    HudVerticalLocator,
    LifeStateRecognizer,
    LifeStateStabilizer,
    PlayerNameBatch,
    PlayerNameOcrWorker,
    PlayerNameStabilizer,
    PortraitChangeGate,
)


@dataclass
class _SlotRuntime:
    side: str
    slot: int
    hero_box: tuple[float, float, float, float]
    name_box: tuple[float, float, float, float]
    status_box: tuple[float, float, float, float]
    hero: HeroStabilizer = field(default_factory=HeroStabilizer)
    life: LifeStateStabilizer = field(default_factory=LifeStateStabilizer)
    player_name: PlayerNameStabilizer = field(default_factory=PlayerNameStabilizer)
    gate: PortraitChangeGate = field(default_factory=PortraitChangeGate)
    events: deque[PlayerEvent] = field(default_factory=lambda: deque(maxlen=6))
    name_image_png: bytes | None = None
    last_name_capture_s: float = float("-inf")


def resource_root() -> Path:
    return Path(__file__).resolve().parent / "resources"


class RealtimeAnalyzer:
    """Incremental analyzer: cheap status each cycle, at most two hero jobs."""

    def __init__(
        self,
        layout: HudLayout = DEFAULT_LAYOUT,
        max_hero_jobs: int = 2,
        *,
        auto_locate_hud: bool = True,
        enable_player_name_ocr: bool = True,
        player_name_worker: PlayerNameOcrWorker | None = None,
    ) -> None:
        resources = resource_root()
        self.layout = layout
        self.max_hero_jobs = max(1, max_hero_jobs)
        self.hero_recognizer = HeroRecognizer(
            resources / "heroes" / "illustrated",
        )
        self.hud_locator = (
            HudVerticalLocator(layout, self.hero_recognizer)
            if auto_locate_hud
            else None
        )
        self._active_hud_offset_y_px: int | None = None
        self.life_recognizer = LifeStateRecognizer()
        self._name_worker = (
            player_name_worker
            if player_name_worker is not None
            else (PlayerNameOcrWorker() if enable_player_name_ocr else None)
        )
        self._last_name_ocr_s = float("-inf")
        self._latest_name_crops: tuple[np.ndarray, ...] = ()
        self.timeline_origin: float | None = None
        self.timeline: deque[TimelineEvent] = deque(maxlen=300)
        self.slots = [
            _SlotRuntime(side, slot, hero_box, name_box, status_box)
            for side, slot, hero_box, name_box, status_box in layout.slots()
        ]

    def analyze(
        self,
        top_frame: np.ndarray,
        full_size: tuple[int, int],
        *,
        captured_at: float | None = None,
        source_name: str = "screen",
        capture_fps: float = 0.0,
        desktop_bounds: tuple[int, int, int, int] | None = None,
    ) -> AnalysisSnapshot:
        started = time.perf_counter()
        now = captured_at if captured_at is not None else time.monotonic()
        if self.timeline_origin is None:
            self.timeline_origin = now
        elapsed_s = max(0.0, now - self.timeline_origin)
        hud_location = (
            self.hud_locator.observe(top_frame, full_size)
            if self.hud_locator is not None
            else HudLocation("locked", 0, 1.0)
        )
        if hud_location.status != "locked" or hud_location.offset_y_px is None:
            return AnalysisSnapshot(
                slots=self._slot_snapshots(),
                captured_at=now,
                cycle_latency_ms=(time.perf_counter() - started) * 1000.0,
                classified_slots=0,
                source_name=source_name,
                capture_fps=capture_fps,
                desktop_bounds=desktop_bounds,
                elapsed_s=elapsed_s,
                timeline=tuple(self.timeline),
                hud_status=hud_location.status,
                hud_offset_y_px=None,
                hud_confidence=hud_location.confidence,
            )
        vertical_offset_px = hud_location.offset_y_px
        if vertical_offset_px != self._active_hud_offset_y_px:
            for runtime in self.slots:
                runtime.gate.fingerprint = None
                runtime.name_image_png = None
                runtime.last_name_capture_s = float("-inf")
                runtime.player_name.clear_candidate()
            self._active_hud_offset_y_px = vertical_offset_px
        self._consume_player_name_result(vertical_offset_px)
        queue: list[tuple[int, _SlotRuntime, np.ndarray]] = []
        name_crops: list[np.ndarray] = []

        for runtime in self.slots:
            name_crop = crop_from_top(
                top_frame,
                full_size,
                runtime.name_box,
                self.layout.logical_aspect_ratio,
                vertical_offset_px,
            )
            name_crops.append(name_crop)
            if runtime.name_image_png is None or now - runtime.last_name_capture_s >= 1.0:
                runtime.name_image_png = self._encode_name_strip(name_crop)
                runtime.last_name_capture_s = now
            status_crop = crop_from_top(
                top_frame,
                full_size,
                runtime.status_box,
                self.layout.logical_aspect_ratio,
                vertical_offset_px,
            )
            previous_life = runtime.life.value
            life_changed = runtime.life.observe(
                self.life_recognizer.match(status_crop, name_crop)
            )
            if life_changed:
                if runtime.life.value == "dead":
                    self._append_event(
                        runtime,
                        PlayerEvent("death", elapsed_s, runtime.hero.value),
                    )
                elif previous_life == "dead" and runtime.life.value == "alive":
                    self._append_event(
                        runtime,
                        PlayerEvent("respawn", elapsed_s, runtime.hero.value),
                    )
            portrait = crop_from_top(
                top_frame,
                full_size,
                runtime.hero_box,
                self.layout.logical_aspect_ratio,
                vertical_offset_px,
            )
            if runtime.gate.should_classify(portrait, now, runtime.hero.pending):
                priority = 0 if runtime.hero.pending else (1 if runtime.hero.value is None else 2)
                queue.append((priority, runtime, portrait))

        self._latest_name_crops = tuple(name_crops)
        if (
            self._name_worker is not None
            and now - self._last_name_ocr_s >= 0.75
            and self._name_worker.submit(
                self._latest_name_crops,
                captured_at=now,
                hud_offset_y_px=vertical_offset_px,
            )
        ):
            self._last_name_ocr_s = now

        queue.sort(key=lambda row: (row[0], row[1].gate.last_classified_s, row[1].slot))
        classified = 0
        for _, runtime, portrait in queue[: self.max_hero_jobs]:
            previous_hero = runtime.hero.value
            hero_changed = runtime.hero.observe(self.hero_recognizer.match(portrait))
            if hero_changed and runtime.hero.value is not None:
                self._append_event(
                    runtime,
                    PlayerEvent(
                        "hero_pick" if previous_hero is None else "hero_switch",
                        elapsed_s,
                        runtime.hero.value,
                        previous_hero,
                    )
                )
            runtime.gate.commit(portrait, now)
            classified += 1

        snapshots = self._slot_snapshots()
        return AnalysisSnapshot(
            slots=snapshots,
            captured_at=now,
            cycle_latency_ms=(time.perf_counter() - started) * 1000.0,
            classified_slots=classified,
            source_name=source_name,
            capture_fps=capture_fps,
            desktop_bounds=desktop_bounds,
            elapsed_s=elapsed_s,
            timeline=tuple(self.timeline),
            hud_status=hud_location.status,
            hud_offset_y_px=vertical_offset_px,
            hud_confidence=hud_location.confidence,
        )

    def _slot_snapshots(self) -> tuple[SlotSnapshot, ...]:
        return tuple(
            SlotSnapshot(
                side=runtime.side,
                slot=runtime.slot,
                hero=runtime.hero.value,
                hero_confidence=runtime.hero.confidence,
                life_state=runtime.life.value,
                life_confidence=runtime.life.confidence,
                pending_hero=runtime.hero.candidate,
                events=tuple(runtime.events),
                player_name=runtime.player_name.value,
                player_name_confidence=runtime.player_name.confidence,
                player_name_status=runtime.player_name.status,
                player_name_image_png=runtime.name_image_png,
            )
            for runtime in self.slots
        )

    def _append_event(self, runtime: _SlotRuntime, event: PlayerEvent) -> None:
        runtime.events.append(event)
        self.timeline.append(
            TimelineEvent(
                side=runtime.side,
                slot=runtime.slot,
                kind=event.kind,
                elapsed_s=event.elapsed_s,
                hero=event.hero,
                previous_hero=event.previous_hero,
            )
        )

    def _consume_player_name_result(self, hud_offset_y_px: int) -> bool:
        if self._name_worker is None:
            return False
        result = self._name_worker.poll()
        if result is None:
            return False
        return self._apply_player_name_result(result, hud_offset_y_px)

    def _apply_player_name_result(
        self,
        result: PlayerNameBatch,
        hud_offset_y_px: int,
    ) -> bool:
        if result.error or result.hud_offset_y_px != hud_offset_y_px:
            return False
        changed = False
        for runtime, match in zip(self.slots, result.matches):
            changed = runtime.player_name.observe(match) or changed
        return changed

    def settle_player_names(
        self,
        *,
        timeout_s: float = 5.0,
        observations: int = 5,
    ) -> None:
        """Finish deterministic OCR for one-shot image analysis only."""
        if (
            self._name_worker is None
            or self._active_hud_offset_y_px is None
            or not self._latest_name_crops
        ):
            return
        deadline = time.monotonic() + max(0.0, timeout_s)
        completed = 0
        while completed < max(1, observations):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            result = self._name_worker.wait(remaining)
            if result is None or result.error:
                return
            self._apply_player_name_result(result, self._active_hud_offset_y_px)
            completed += 1
            if all(runtime.player_name.value is not None for runtime in self.slots):
                return
            if completed >= observations:
                return
            self._name_worker.submit(
                self._latest_name_crops,
                captured_at=time.monotonic(),
                hud_offset_y_px=self._active_hud_offset_y_px,
            )

    def close(self) -> None:
        if self._name_worker is not None:
            self._name_worker.close()
            self._name_worker = None

    @staticmethod
    def _encode_name_strip(crop: np.ndarray) -> bytes | None:
        """Keep the exact HUD name visible without putting slow OCR in the hot path."""
        if crop.size == 0:
            return None
        ok, encoded = cv2.imencode(".png", crop)
        return encoded.tobytes() if ok else None
