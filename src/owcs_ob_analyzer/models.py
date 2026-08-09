from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal

LifeState = Literal["alive", "dead", "unknown"]
HudStatus = Literal["searching", "locked"]
PlayerNameStatus = Literal["confirmed", "unknown"]
PlayerEventKind = Literal["hero_pick", "hero_switch", "death", "respawn"]


@dataclass(frozen=True)
class HeroMatch:
    hero: str | None
    confidence: float
    candidates: list[tuple[str, float]] = field(default_factory=list)


@dataclass(frozen=True)
class LifeMatch:
    state: LifeState
    confidence: float
    alive_score: float = 0.0
    death_score: float = 0.0


@dataclass(frozen=True)
class PlayerEvent:
    kind: PlayerEventKind
    elapsed_s: float
    hero: str | None = None
    previous_hero: str | None = None


@dataclass(frozen=True)
class TimelineEvent:
    side: str
    slot: int
    kind: PlayerEventKind
    elapsed_s: float
    hero: str | None = None
    previous_hero: str | None = None


@dataclass(frozen=True)
class SlotSnapshot:
    side: str
    slot: int
    hero: str | None
    hero_confidence: float
    life_state: LifeState
    life_confidence: float
    pending_hero: str | None = None
    events: tuple[PlayerEvent, ...] = ()
    player_name: str | None = None
    player_name_confidence: float = 0.0
    player_name_status: PlayerNameStatus = "unknown"
    player_name_image_png: bytes | None = None


@dataclass(frozen=True)
class AnalysisSnapshot:
    slots: tuple[SlotSnapshot, ...]
    captured_at: float
    cycle_latency_ms: float
    classified_slots: int
    source_name: str
    capture_fps: float = 0.0
    desktop_bounds: tuple[int, int, int, int] | None = None
    elapsed_s: float = 0.0
    timeline: tuple[TimelineEvent, ...] = ()
    hud_status: HudStatus = "locked"
    hud_offset_y_px: int | None = 0
    hud_confidence: float = 1.0

    def as_dict(self) -> dict[str, object]:
        return {
            "captured_at": self.captured_at,
            "cycle_latency_ms": round(self.cycle_latency_ms, 1),
            "classified_slots": self.classified_slots,
            "source": self.source_name,
            "capture_fps": round(self.capture_fps, 1),
            "desktop_bounds": self.desktop_bounds,
            "elapsed_s": round(self.elapsed_s, 3),
            "hud": {
                "status": self.hud_status,
                "offset_y_px": self.hud_offset_y_px,
                "confidence": round(self.hud_confidence, 4),
            },
            "timeline": [
                {
                    "side": event.side,
                    "slot": event.slot,
                    "kind": event.kind,
                    "elapsed_s": round(event.elapsed_s, 3),
                    "hero": event.hero,
                    "previous_hero": event.previous_hero,
                }
                for event in self.timeline
            ],
            "slots": [
                {
                    "side": row.side,
                    "slot": row.slot,
                    "hero": row.hero,
                    "hero_confidence": round(row.hero_confidence, 4),
                    "life_state": row.life_state,
                    "life_confidence": round(row.life_confidence, 4),
                    "pending_hero": row.pending_hero,
                    "player_name": row.player_name,
                    "player_name_confidence": round(row.player_name_confidence, 4),
                    "player_name_status": row.player_name_status,
                    "events": [
                        {
                            "kind": event.kind,
                            "elapsed_s": round(event.elapsed_s, 3),
                            "hero": event.hero,
                            "previous_hero": event.previous_hero,
                        }
                        for event in row.events
                    ],
                }
                for row in self.slots
            ],
        }
