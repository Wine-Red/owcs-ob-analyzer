from __future__ import annotations

import asyncio
import json
from pathlib import Path

from aiohttp import ClientSession

from owcs_ob_analyzer.models import AnalysisSnapshot, SlotSnapshot
from owcs_ob_analyzer.state_output import PlayerStateStore, StateOutputServer


def _snapshot(
    *,
    hero: str | None = "ana",
    life_state: str = "alive",
    hud_status: str = "locked",
) -> AnalysisSnapshot:
    slots = tuple(
        SlotSnapshot(
            side=side,
            slot=slot,
            hero=hero,
            hero_confidence=0.96,
            life_state=life_state,  # type: ignore[arg-type]
            life_confidence=0.99,
            pending_hero="kiriko",
            player_name="PLAYER" if side == "left" and slot == 0 else None,
            player_name_confidence=0.97 if side == "left" and slot == 0 else 0.0,
            player_name_status="confirmed" if side == "left" and slot == 0 else "unknown",
        )
        for side in ("left", "right")
        for slot in range(5)
    )
    return AnalysisSnapshot(
        slots=slots,
        captured_at=10.0,
        cycle_latency_ms=12.4,
        classified_slots=2,
        source_name="test-source",
        capture_fps=5.0,
        hud_status=hud_status,  # type: ignore[arg-type]
        hud_offset_y_px=0 if hud_status == "locked" else None,
        hud_confidence=0.98 if hud_status == "locked" else 0.0,
    )


def test_store_publishes_only_confirmed_material_state_changes() -> None:
    store = PlayerStateStore()
    first = store.ingest(_snapshot())
    assert first is not None
    assert first["sequence"] == 1
    assert len(first["players"]) == 10
    assert first["players"][0]["slot_id"] == "left-1"
    assert first["players"][9]["slot_id"] == "right-5"
    assert first["hud"]["status"] == "locked"
    assert first["hud"]["offset_y_px"] == 0
    assert first["players"][0]["display_name"] == "PLAYER"
    assert first["players"][0]["display_name_status"] == "confirmed"
    assert first["players"][0]["display_name_confidence"] == 0.97
    assert "pending_hero" not in first["players"][0]

    # A repeated analyzer cycle and its pending candidate are not public changes.
    assert store.ingest(_snapshot()) is None

    locating = store.ingest(_snapshot(hud_status="searching"))
    assert locating is not None
    assert locating["sequence"] == 2
    assert locating["hud"]["status"] == "searching"

    changed = store.ingest(_snapshot(hero="kiriko", life_state="dead"))
    assert changed is not None
    assert changed["sequence"] == 3
    assert changed["players"][0]["hero"] == "kiriko"
    assert changed["players"][0]["life_state"] == "dead"


def test_unknown_is_explicit_and_never_guessed() -> None:
    store = PlayerStateStore()
    payload = store.ingest(_snapshot(hero=None, life_state="unknown"))
    assert payload is not None
    player = payload["players"][0]
    assert player["hero"] is None
    assert player["hero_status"] == "unknown"
    assert player["life_state"] == "unknown"
    assert player["life_status"] == "unknown"


def test_http_snapshot_and_websocket_change_stream() -> None:
    async def scenario() -> None:
        server = StateOutputServer(
            "127.0.0.1",
            0,
            heartbeat_interval_s=10.0,
        )
        server.start()
        try:
            async with ClientSession() as client:
                health_response = await client.get(f"{server.http_url}/health")
                assert health_response.status == 200
                assert health_response.headers["Access-Control-Allow-Origin"] == "*"
                assert health_response.headers["Cache-Control"] == "no-store"
                health = await health_response.json()
                assert health["source_status"] == "waiting"

                websocket = await client.ws_connect(
                    f"{server.http_url}/v1/state-stream"
                )
                unavailable = await websocket.receive_json(timeout=1.0)
                assert unavailable["type"] == "state.unavailable"

                assert server.publish(_snapshot()) is True
                streamed = await websocket.receive_json(timeout=1.0)
                assert streamed["type"] == "state.snapshot"
                assert streamed["players"][0]["hero"] == "ana"

                state_response = await client.get(f"{server.http_url}/v1/state")
                assert state_response.status == 200
                current = await state_response.json()
                assert current["sequence"] == streamed["sequence"]
                assert current["source_status"] == "online"
                await websocket.close()
        finally:
            server.stop()

    asyncio.run(scenario())


def test_snapshot_schema_is_shipped_and_describes_ten_players() -> None:
    schema_path = Path(__file__).parents[1] / "schema" / "player-state-v1.schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    assert schema["$schema"].endswith("2020-12/schema")
    assert schema["properties"]["players"]["minItems"] == 10
    assert schema["properties"]["players"]["maxItems"] == 10
