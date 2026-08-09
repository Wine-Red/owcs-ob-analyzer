from __future__ import annotations

from dataclasses import dataclass

NormalizedBox = tuple[float, float, float, float]


@dataclass(frozen=True)
class HudLayout:
    """Custom-spectator HUD coordinates on Overwatch's logical canvas."""

    top_capture_ratio: float
    logical_aspect_ratio: float
    hero_rois: dict[str, tuple[NormalizedBox, ...]]
    player_name_rois: dict[str, tuple[NormalizedBox, ...]]
    status_rois: dict[str, tuple[NormalizedBox, ...]]

    def slots(
        self,
    ) -> tuple[tuple[str, int, NormalizedBox, NormalizedBox, NormalizedBox], ...]:
        return tuple(
            (
                side,
                slot,
                hero_box,
                self.player_name_rois[side][slot],
                self.status_rois[side][slot],
            )
            for side in ("left", "right")
            for slot, hero_box in enumerate(self.hero_rois[side])
        )


# Borderless Overwatch can expose a 16:10 Windows client while its in-game
# aspect setting remains 16:9. These coordinates therefore use the game's
# logical 16:9 HUD canvas, not the physical client height reported by DXGI.
DEFAULT_LAYOUT = HudLayout(
    # Include the broadcast-safe area used by tournament observers. The
    # logical 16:9 geometry remains unchanged; only a vertical translation is
    # discovered inside this larger, still top-only capture.
    top_capture_ratio=0.24,
    logical_aspect_ratio=16 / 9,
    hero_rois={
        "left": (
            (0.047, 0.0674, 0.074, 0.1029),
            (0.102, 0.0674, 0.128, 0.1029),
            (0.157, 0.0674, 0.183, 0.1029),
            (0.212, 0.0674, 0.238, 0.1029),
            (0.268, 0.0674, 0.295, 0.1029),
        ),
        "right": (
            (0.730, 0.0674, 0.757, 0.1029),
            (0.785, 0.0674, 0.812, 0.1029),
            (0.841, 0.0674, 0.868, 0.1029),
            (0.896, 0.0674, 0.923, 0.1029),
            (0.951, 0.0674, 0.978, 0.1029),
        ),
    },
    player_name_rois={
        "left": (
            (0.022, 0.1029, 0.074, 0.1237),
            (0.078, 0.1029, 0.128, 0.1237),
            (0.131, 0.1029, 0.183, 0.1237),
            (0.187, 0.1029, 0.238, 0.1237),
            (0.242, 0.1029, 0.295, 0.1237),
        ),
        "right": (
            (0.705, 0.1029, 0.757, 0.1237),
            (0.760, 0.1029, 0.812, 0.1237),
            (0.816, 0.1029, 0.868, 0.1237),
            (0.871, 0.1029, 0.923, 0.1237),
            (0.926, 0.1029, 0.978, 0.1237),
        ),
    },
    status_rois={
        "left": (
            (0.022, 0.1237, 0.074, 0.1460),
            (0.078, 0.1237, 0.128, 0.1460),
            (0.131, 0.1237, 0.183, 0.1460),
            (0.187, 0.1237, 0.238, 0.1460),
            (0.242, 0.1237, 0.295, 0.1460),
        ),
        "right": (
            (0.705, 0.1237, 0.757, 0.1460),
            (0.760, 0.1237, 0.812, 0.1460),
            (0.816, 0.1237, 0.868, 0.1460),
            (0.871, 0.1237, 0.923, 0.1460),
            (0.926, 0.1237, 0.978, 0.1460),
        ),
    },
)


def crop_from_top(
    top_frame: object,
    full_size: tuple[int, int],
    box: NormalizedBox,
    logical_aspect_ratio: float = 16 / 9,
    vertical_offset_px: int = 0,
):
    """Crop a logical-HUD box from a top-only physical client frame."""
    width, physical_height = full_size
    logical_height = min(physical_height, round(width / logical_aspect_ratio))
    x1, y1, x2, y2 = box
    left = max(0, min(top_frame.shape[1], round(x1 * width)))
    right = max(0, min(top_frame.shape[1], round(x2 * width)))
    top = max(
        0,
        min(top_frame.shape[0], round(y1 * logical_height) + vertical_offset_px),
    )
    bottom = max(
        0,
        min(top_frame.shape[0], round(y2 * logical_height) + vertical_offset_px),
    )
    return top_frame[top:bottom, left:right]
