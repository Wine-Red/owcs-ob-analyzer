from __future__ import annotations

import asyncio
from copy import deepcopy
from datetime import datetime, timezone
import threading
import time
from typing import Any
from uuid import uuid4

from aiohttp import WSMsgType, web

from .models import AnalysisSnapshot, SlotSnapshot

SCHEMA_VERSION = "1.0"


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


class PlayerStateStore:
    """Thread-safe canonical state derived only from confirmed analyzer values."""

    def __init__(self, *, stale_after_s: float = 2.5) -> None:
        self.stale_after_s = max(0.1, stale_after_s)
        self._lock = threading.Lock()
        self._session_id = uuid4().hex
        self._sequence = 0
        self._fingerprint: tuple[object, ...] | None = None
        self._slot_fingerprints: dict[str, tuple[object, ...]] = {}
        self._slot_updated_at: dict[str, str] = {}
        self._current: dict[str, Any] | None = None
        self._last_source_seen: float | None = None
        self._source_status = "waiting"
        self._source_error: str | None = None

    def begin_session(self) -> dict[str, Any]:
        with self._lock:
            self._session_id = uuid4().hex
            self._sequence = 0
            self._fingerprint = None
            self._slot_fingerprints.clear()
            self._slot_updated_at.clear()
            self._current = None
            self._last_source_seen = None
            self._source_status = "waiting"
            self._source_error = None
            return {
                "type": "session.reset",
                "schema_version": SCHEMA_VERSION,
                "session_id": self._session_id,
                "sequence": self._sequence,
                "observed_at": _utc_now(),
            }

    def ingest(self, snapshot: AnalysisSnapshot) -> dict[str, Any] | None:
        now_monotonic = time.monotonic()
        now_utc = _utc_now()
        with self._lock:
            self._last_source_seen = now_monotonic
            self._source_status = "online"
            self._source_error = None
            fingerprint = (
                snapshot.hud_status,
                snapshot.hud_offset_y_px if snapshot.hud_status == "locked" else None,
                *(self._slot_fingerprint(row) for row in snapshot.slots),
            )
            if fingerprint == self._fingerprint:
                return None

            players = []
            for row in snapshot.slots:
                slot_id = self._slot_id(row)
                slot_fingerprint = self._slot_fingerprint(row)
                if self._slot_fingerprints.get(slot_id) != slot_fingerprint:
                    self._slot_updated_at[slot_id] = now_utc
                self._slot_fingerprints[slot_id] = slot_fingerprint
                players.append(self._player_payload(row, self._slot_updated_at[slot_id]))

            self._fingerprint = fingerprint
            self._sequence += 1
            self._current = {
                "type": "state.snapshot",
                "schema_version": SCHEMA_VERSION,
                "session_id": self._session_id,
                "sequence": self._sequence,
                "observed_at": now_utc,
                "source_age_ms": 0,
                "source_status": "online",
                "hud": {
                    "status": snapshot.hud_status,
                    "offset_y_px": snapshot.hud_offset_y_px,
                    "confidence": round(snapshot.hud_confidence, 4),
                },
                "source": {
                    "name": snapshot.source_name,
                    "capture_fps": round(snapshot.capture_fps, 1),
                    "cycle_latency_ms": round(snapshot.cycle_latency_ms, 1),
                },
                "players": players,
            }
            return deepcopy(self._current)

    def current_state(self) -> dict[str, Any] | None:
        with self._lock:
            if self._current is None:
                return None
            payload = deepcopy(self._current)
            payload["source_age_ms"] = self._source_age_ms_locked()
            payload["source_status"] = self._effective_status_locked()
            return payload

    def unavailable(self) -> dict[str, Any]:
        with self._lock:
            return {
                "type": "state.unavailable",
                "schema_version": SCHEMA_VERSION,
                "session_id": self._session_id,
                "sequence": self._sequence,
                "observed_at": _utc_now(),
                "source_age_ms": self._source_age_ms_locked(),
                "source_status": self._effective_status_locked(),
                "error": self._source_error,
            }

    def heartbeat(self) -> dict[str, Any]:
        with self._lock:
            return {
                "type": "heartbeat",
                "schema_version": SCHEMA_VERSION,
                "session_id": self._session_id,
                "sequence": self._sequence,
                "server_time": _utc_now(),
                "source_age_ms": self._source_age_ms_locked(),
                "source_status": self._effective_status_locked(),
            }

    def health(self) -> dict[str, Any]:
        heartbeat = self.heartbeat()
        return {
            "service": "owcs-ob-analyzer",
            "status": "ok",
            **heartbeat,
        }

    def mark_source_stopped(self, error: str | None = None) -> dict[str, Any]:
        with self._lock:
            if error:
                self._source_status = "error"
                self._source_error = error
            elif self._source_status != "error":
                self._source_status = "stopped"
                self._source_error = None
            return {
                "type": "source.status",
                "schema_version": SCHEMA_VERSION,
                "session_id": self._session_id,
                "sequence": self._sequence,
                "observed_at": _utc_now(),
                "source_age_ms": self._source_age_ms_locked(),
                "source_status": self._source_status,
                "error": self._source_error,
            }

    def _source_age_ms_locked(self) -> int | None:
        if self._last_source_seen is None:
            return None
        return max(0, round((time.monotonic() - self._last_source_seen) * 1000))

    def _effective_status_locked(self) -> str:
        if self._source_status == "online":
            age_ms = self._source_age_ms_locked()
            if age_ms is not None and age_ms > self.stale_after_s * 1000:
                return "stale"
        return self._source_status

    @staticmethod
    def _slot_id(row: SlotSnapshot) -> str:
        return f"{row.side}-{row.slot + 1}"

    @staticmethod
    def _slot_fingerprint(row: SlotSnapshot) -> tuple[object, ...]:
        # Deliberately excludes confidence and pending_hero. Neither may cause a
        # broadcast-facing state change before the stabilizers confirm it.
        return (row.side, row.slot, row.player_name, row.hero, row.life_state)

    @classmethod
    def _player_payload(cls, row: SlotSnapshot, updated_at: str) -> dict[str, Any]:
        return {
            "slot_id": cls._slot_id(row),
            "side": row.side,
            "slot": row.slot + 1,
            "display_name": row.player_name,
            "display_name_status": row.player_name_status,
            "display_name_confidence": round(row.player_name_confidence, 4),
            "hero": row.hero,
            "life_state": row.life_state,
            "hero_status": "confirmed" if row.hero is not None else "unknown",
            "life_status": "confirmed" if row.life_state != "unknown" else "unknown",
            "hero_confidence": round(row.hero_confidence, 4),
            "life_confidence": round(row.life_confidence, 4),
            "updated_at": updated_at,
        }


class StateOutputServer:
    """Small LAN HTTP/WebSocket server hosted on a dedicated asyncio thread."""

    def __init__(
        self,
        host: str = "0.0.0.0",
        port: int = 8765,
        *,
        heartbeat_interval_s: float = 1.0,
        stale_after_s: float = 2.5,
    ) -> None:
        self.host = host
        self.port = port
        self.heartbeat_interval_s = max(0.05, heartbeat_interval_s)
        self.store = PlayerStateStore(stale_after_s=stale_after_s)
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._stop_event: asyncio.Event | None = None
        self._outbound: asyncio.Queue[dict[str, Any]] | None = None
        self._clients: set[web.WebSocketResponse] = set()
        self._ready = threading.Event()
        self._startup_error: BaseException | None = None

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    @property
    def http_url(self) -> str:
        host = "127.0.0.1" if self.host in {"0.0.0.0", "::"} else self.host
        return f"http://{host}:{self.port}"

    def start(self, timeout_s: float = 5.0) -> None:
        if self.is_running:
            return
        self._ready.clear()
        self._startup_error = None
        self._thread = threading.Thread(
            target=self._thread_main,
            name="owcs-state-output",
            daemon=True,
        )
        self._thread.start()
        if not self._ready.wait(timeout_s):
            raise RuntimeError("state output server did not start in time")
        if self._startup_error is not None:
            error = self._startup_error
            self._thread.join(timeout=1.0)
            self._thread = None
            raise RuntimeError(f"state output server failed: {error}") from error

    def stop(self, timeout_s: float = 5.0) -> None:
        loop = self._loop
        stop_event = self._stop_event
        if loop is not None and stop_event is not None and loop.is_running():
            loop.call_soon_threadsafe(stop_event.set)
        thread = self._thread
        if thread is not None:
            thread.join(timeout=timeout_s)
        self._thread = None

    def begin_session(self) -> None:
        self._enqueue(self.store.begin_session())

    def publish(self, snapshot: AnalysisSnapshot) -> bool:
        payload = self.store.ingest(snapshot)
        if payload is None:
            return False
        self._enqueue(payload)
        return True

    def mark_source_stopped(self, error: str | None = None) -> None:
        self._enqueue(self.store.mark_source_stopped(error))

    def _enqueue(self, payload: dict[str, Any]) -> None:
        loop = self._loop
        if loop is not None and loop.is_running():
            loop.call_soon_threadsafe(self._enqueue_on_loop, payload)

    def _enqueue_on_loop(self, payload: dict[str, Any]) -> None:
        if self._outbound is None:
            return
        try:
            self._outbound.put_nowait(payload)
        except asyncio.QueueFull:
            # Messages are full snapshots; preserving the newest state is safer
            # than allowing a slow consumer to build an unbounded backlog.
            try:
                self._outbound.get_nowait()
            except asyncio.QueueEmpty:
                pass
            self._outbound.put_nowait(payload)

    def _thread_main(self) -> None:
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._serve())
        except BaseException as exc:
            self._startup_error = exc
            self._ready.set()
        finally:
            self._loop = None
            loop.close()

    async def _serve(self) -> None:
        self._stop_event = asyncio.Event()
        self._outbound = asyncio.Queue(maxsize=32)
        app = web.Application()
        app.router.add_get("/health", self._handle_health)
        app.router.add_get("/v1/state", self._handle_state)
        app.router.add_get("/v1/state-stream", self._handle_websocket)
        runner = web.AppRunner(app)
        await runner.setup()
        try:
            site = web.TCPSite(runner, self.host, self.port)
            await site.start()
            if self.port == 0 and site._server is not None and site._server.sockets:
                self.port = int(site._server.sockets[0].getsockname()[1])
            sender = asyncio.create_task(self._send_outbound())
            heartbeat = asyncio.create_task(self._send_heartbeats())
            self._ready.set()
            await self._stop_event.wait()
            sender.cancel()
            heartbeat.cancel()
            await asyncio.gather(sender, heartbeat, return_exceptions=True)
            await self._close_clients()
        finally:
            await runner.cleanup()

    async def _handle_health(self, _request: web.Request) -> web.Response:
        return self._json_response(self.store.health())

    async def _handle_state(self, _request: web.Request) -> web.Response:
        payload = self.store.current_state()
        if payload is None:
            return self._json_response(self.store.unavailable(), status=503)
        return self._json_response(payload)

    async def _handle_websocket(self, request: web.Request) -> web.WebSocketResponse:
        websocket = web.WebSocketResponse(heartbeat=20.0)
        await websocket.prepare(request)
        self._clients.add(websocket)
        initial = self.store.current_state() or self.store.unavailable()
        await websocket.send_json(initial)
        try:
            async for message in websocket:
                if message.type == WSMsgType.ERROR:
                    break
        finally:
            self._clients.discard(websocket)
        return websocket

    async def _send_outbound(self) -> None:
        assert self._outbound is not None
        while True:
            payload = await self._outbound.get()
            await self._broadcast(payload)

    async def _send_heartbeats(self) -> None:
        while True:
            await asyncio.sleep(self.heartbeat_interval_s)
            await self._broadcast(self.store.heartbeat())

    async def _broadcast(self, payload: dict[str, Any]) -> None:
        if not self._clients:
            return
        clients = tuple(self._clients)
        results = await asyncio.gather(
            *(client.send_json(payload) for client in clients),
            return_exceptions=True,
        )
        for client, result in zip(clients, results):
            if isinstance(result, BaseException) or client.closed:
                self._clients.discard(client)

    async def _close_clients(self) -> None:
        clients = tuple(self._clients)
        self._clients.clear()
        await asyncio.gather(
            *(client.close(code=1001, message=b"service stopping") for client in clients),
            return_exceptions=True,
        )

    @staticmethod
    def _json_response(payload: dict[str, Any], *, status: int = 200) -> web.Response:
        return web.json_response(
            payload,
            status=status,
            headers={
                "Access-Control-Allow-Origin": "*",
                "Cache-Control": "no-store",
            },
        )
