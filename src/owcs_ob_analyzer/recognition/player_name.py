from __future__ import annotations

from dataclasses import dataclass
import importlib.util
import math
from pathlib import Path
import queue
import threading
import time
import unicodedata
from typing import Callable, Protocol

import cv2
import numpy as np


@dataclass(frozen=True)
class PlayerNameMatch:
    text: str | None
    confidence: float


@dataclass(frozen=True)
class PlayerNameBatch:
    captured_at: float
    hud_offset_y_px: int
    matches: tuple[PlayerNameMatch, ...]
    error: str | None = None


def normalize_player_id(value: str) -> str:
    """Normalize OCR formatting without changing a player's visible ID."""
    normalized = unicodedata.normalize("NFKC", value)
    normalized = "".join(
        character
        for character in normalized
        if unicodedata.category(character) not in {"Cc", "Cf"}
        and not character.isspace()
    )
    return normalized.strip("|[]{}()<>《》【】")


def _comparison_key(value: str) -> str:
    return normalize_player_id(value).casefold()


def _valid_player_id(value: str) -> bool:
    if not 1 <= len(value) <= 32:
        return False
    return any(character.isalnum() for character in value)


class _RapidOcrEngine(Protocol):
    def __call__(self, image: np.ndarray, **kwargs: object) -> object: ...


def _recognition_model_path() -> Path:
    bundled = (
        Path(__file__).resolve().parents[1]
        / "resources"
        / "ocr"
        / "ch_PP-OCRv4_rec_infer.onnx"
    )
    if bundled.is_file():
        return bundled

    # Source/editable installs use rapidocr-onnxruntime only as the licensed
    # supplier of the compact multilingual recognition model. The packaged
    # application copies this one model into our resources and does not bundle
    # RapidOCR's unused detector, orientation classifier, or visualization
    # dependencies.
    package = importlib.util.find_spec("rapidocr_onnxruntime")
    locations = package.submodule_search_locations if package is not None else None
    if locations:
        model = (
            Path(next(iter(locations)))
            / "models"
            / "ch_PP-OCRv4_rec_infer.onnx"
        )
        if model.is_file():
            return model
    raise RuntimeError("未找到选手 ID 识别模型 ch_PP-OCRv4_rec_infer.onnx")


class _RecognitionOnlyOnnxEngine:
    """Minimal PP-OCR recognizer without detection or image utilities."""

    def __init__(self, model_path: Path | None = None) -> None:
        import onnxruntime as ort

        options = ort.SessionOptions()
        options.log_severity_level = 4
        options.enable_cpu_mem_arena = False
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.session = ort.InferenceSession(
            str(model_path or _recognition_model_path()),
            sess_options=options,
            providers=["CPUExecutionProvider"],
        )
        self.input_name = self.session.get_inputs()[0].name
        character = self.session.get_modelmeta().custom_metadata_map.get("character")
        if not character:
            raise RuntimeError("选手 ID 识别模型缺少字符表")
        self.characters = ["blank", *character.splitlines(), " "]

    def __call__(self, image: np.ndarray, **_kwargs: object) -> object:
        started = time.perf_counter()
        text, confidence = self._recognize(image)
        return ([[text, confidence]], time.perf_counter() - started)

    def _recognize(self, image: np.ndarray) -> tuple[str, float]:
        if image.ndim == 2:
            image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
        elif image.ndim == 3 and image.shape[2] == 4:
            image = cv2.cvtColor(image, cv2.COLOR_BGRA2BGR)

        target_height = 48
        target_width = 320
        ratio = image.shape[1] / float(max(1, image.shape[0]))
        canvas_width = max(target_width, int(target_height * ratio))
        resized_width = min(canvas_width, int(math.ceil(target_height * ratio)))
        resized = cv2.resize(image, (max(1, resized_width), target_height))
        normalized = resized.astype(np.float32).transpose((2, 0, 1)) / 255.0
        normalized = (normalized - 0.5) / 0.5
        batch = np.zeros((1, 3, target_height, canvas_width), dtype=np.float32)
        batch[0, :, :, : normalized.shape[2]] = normalized

        prediction = self.session.run(None, {self.input_name: batch})[0][0]
        indices = prediction.argmax(axis=1)
        probabilities = prediction.max(axis=1)
        selected = np.ones(indices.shape[0], dtype=bool)
        selected[1:] = indices[1:] != indices[:-1]
        selected &= indices != 0
        if not np.any(selected):
            return "", 0.0
        text = "".join(
            self.characters[int(index)]
            for index in indices[selected]
            if int(index) < len(self.characters)
        )
        return text, float(np.mean(probabilities[selected]))


class RapidOcrPlayerNameRecognizer:
    """Offline Chinese/English recognition with two-pass agreement.

    The compact PP-OCR Chinese recognition model also covers Latin player IDs.
    A result is accepted only when the original and grayscale name strips
    agree after conservative normalization.
    """

    def __init__(
        self,
        engine: _RapidOcrEngine | None = None,
        *,
        minimum_confidence: float = 0.88,
        fallback_confidence: float = 0.72,
    ) -> None:
        self._engine = engine
        self.minimum_confidence = minimum_confidence
        self.fallback_confidence = fallback_confidence

    def _get_engine(self) -> _RapidOcrEngine:
        if self._engine is None:
            self._engine = _RecognitionOnlyOnnxEngine()
        return self._engine

    @staticmethod
    def _parse_result(result: object) -> tuple[str, float]:
        # Recognition-only RapidOCR returns ([[text, confidence]], elapsed).
        payload = result[0] if isinstance(result, tuple) else result
        if not isinstance(payload, list) or not payload:
            return "", 0.0
        row = payload[0]
        if not isinstance(row, (list, tuple)) or len(row) < 2:
            return "", 0.0
        try:
            return str(row[0]), float(row[1])
        except (TypeError, ValueError):
            return "", 0.0

    def recognize(self, image: np.ndarray) -> PlayerNameMatch:
        if image.size == 0:
            return PlayerNameMatch(None, 0.0)
        engine = self._get_engine()
        variants = (image, cv2.cvtColor(image, cv2.COLOR_BGR2GRAY))
        readings: list[tuple[str, float]] = []
        for variant in variants:
            raw = engine(
                variant,
                use_det=False,
                use_cls=False,
                use_rec=True,
            )
            text, confidence = self._parse_result(raw)
            text = normalize_player_id(text)
            readings.append((text, confidence))

        first, second = readings
        confidence = min(first[1], second[1])
        strongest_confidence = max(first[1], second[1])
        if (
            not _valid_player_id(first[0])
            or _comparison_key(first[0]) != _comparison_key(second[0])
            or (
                confidence < self.minimum_confidence
                and (
                    confidence < self.fallback_confidence
                    or strongest_confidence < 0.85
                )
            )
        ):
            return PlayerNameMatch(None, confidence)
        # Preserve the spelling/case from the stronger pass.
        text = max(readings, key=lambda row: row[1])[0]
        return PlayerNameMatch(text, confidence)

    def recognize_many(
        self,
        images: tuple[np.ndarray, ...],
    ) -> tuple[PlayerNameMatch, ...]:
        return tuple(self.recognize(image) for image in images)


@dataclass
class PlayerNameStabilizer:
    """Publish IDs only after repeated exact agreement; never flicker on loss."""

    value: str | None = None
    confidence: float = 0.0
    candidate: str | None = None
    candidate_confidence: float = 0.0
    candidate_count: int = 0
    candidate_misses: int = 0

    @property
    def status(self) -> str:
        return "confirmed" if self.value is not None else "unknown"

    def clear_candidate(self) -> None:
        self.candidate = None
        self.candidate_confidence = 0.0
        self.candidate_count = 0
        self.candidate_misses = 0

    def observe(self, match: PlayerNameMatch) -> bool:
        text = normalize_player_id(match.text or "")
        if not _valid_player_id(text) or match.confidence < 0.72:
            self.candidate_misses += 1
            if self.candidate_misses >= 2:
                self.clear_candidate()
            return False

        key = _comparison_key(text)
        if self.value is not None and key == _comparison_key(self.value):
            self.confidence = max(self.confidence * 0.85, match.confidence)
            self.clear_candidate()
            return False

        if self.candidate is not None and key == _comparison_key(self.candidate):
            self.candidate_count += 1
            if match.confidence >= self.candidate_confidence:
                self.candidate = text
            self.candidate_confidence += (
                match.confidence - self.candidate_confidence
            ) / self.candidate_count
        else:
            self.candidate = text
            self.candidate_confidence = match.confidence
            self.candidate_count = 1
        self.candidate_misses = 0

        # IDs do not legitimately change during a map. Initial recognition is
        # conservative; replacing an existing confirmed ID is stricter still.
        high_confidence = self.candidate_confidence >= 0.88
        if self.value is None:
            required = 3 if high_confidence else 5
        else:
            required = 5 if high_confidence else 8
        minimum_average = 0.88 if high_confidence else 0.72
        if (
            self.candidate_count < required
            or self.candidate_confidence < minimum_average
        ):
            return False
        self.value = self.candidate
        self.confidence = self.candidate_confidence
        self.clear_candidate()
        return True


RecognizerFactory = Callable[[], RapidOcrPlayerNameRecognizer]


class PlayerNameOcrWorker:
    """Single-slot daemon worker so OCR never delays the realtime hot path."""

    def __init__(self, recognizer_factory: RecognizerFactory | None = None) -> None:
        self._recognizer_factory = recognizer_factory or RapidOcrPlayerNameRecognizer
        self._jobs: queue.Queue[
            tuple[float, int, tuple[np.ndarray, ...]] | None
        ] = queue.Queue(maxsize=1)
        self._results: queue.Queue[PlayerNameBatch] = queue.Queue(maxsize=1)
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="owcs-player-name-ocr",
            daemon=True,
        )
        self._thread.start()

    def submit(
        self,
        images: tuple[np.ndarray, ...],
        *,
        captured_at: float,
        hud_offset_y_px: int,
    ) -> bool:
        if self._stop.is_set():
            return False
        try:
            self._jobs.put_nowait(
                (
                    captured_at,
                    hud_offset_y_px,
                    tuple(np.ascontiguousarray(image.copy()) for image in images),
                )
            )
        except queue.Full:
            return False
        return True

    def poll(self) -> PlayerNameBatch | None:
        latest = None
        while True:
            try:
                latest = self._results.get_nowait()
            except queue.Empty:
                return latest

    def wait(self, timeout_s: float) -> PlayerNameBatch | None:
        try:
            return self._results.get(timeout=max(0.0, timeout_s))
        except queue.Empty:
            return None

    def close(self) -> None:
        self._stop.set()
        try:
            self._jobs.put_nowait(None)
        except queue.Full:
            pass
        self._thread.join(timeout=1.0)

    def _publish(self, result: PlayerNameBatch) -> None:
        try:
            self._results.put_nowait(result)
        except queue.Full:
            try:
                self._results.get_nowait()
            except queue.Empty:
                pass
            self._results.put_nowait(result)

    def _run(self) -> None:
        recognizer: RapidOcrPlayerNameRecognizer | None = None
        while not self._stop.is_set():
            try:
                job = self._jobs.get(timeout=0.1)
            except queue.Empty:
                continue
            if job is None:
                return
            captured_at, hud_offset_y_px, images = job
            try:
                if recognizer is None:
                    recognizer = self._recognizer_factory()
                matches = recognizer.recognize_many(images)
                result = PlayerNameBatch(
                    captured_at,
                    hud_offset_y_px,
                    matches,
                )
            except Exception as exc:
                result = PlayerNameBatch(
                    captured_at,
                    hud_offset_y_px,
                    tuple(PlayerNameMatch(None, 0.0) for _ in images),
                    str(exc),
                )
            self._publish(result)
