# Real-game life and identity regression frames

Captured on 2026-10-16 from the user's 2560 × 1600 replay. These are native
game/OBS input pixels, not resized OBS preview screenshots. Pixels outside the
scoreboard and an eight-pixel margin are blacked out to keep fixtures small;
the original dimensions, scoreboard position and pixels are unchanged.
`expected.json` records source filenames and original SHA256 hashes.

Human-reviewed cases:

- `before-swap`: VICTORIA on vendetta, everyone alive; both other damage players
  still use symmetra.
- `white-portrait`: VICTORIA changes to shion, alive. White portrait detail must
  not become a death ring or an invented countdown of 8.
- `death-cross`: player 800 has the initial death cross, without a countdown.
- `expanded-cross`: 800 has countdown 8; IR1S has an expanding death cross. That
  animation obscures part of DAMO's portrait; the current observation is unknown,
  while the state keeps DAMO's earlier alive reading without refreshing it.
- `countdown`: 800 and IR1S both have countdown 4.
- `heldout-deaths`: later replay sample with countdown 5 on both players.
- `cassidy-swap`: later replay sample, everyone alive, VICTORIA on cassidy.
- `heldout-nine`: EZHAN has countdown 9; all other players are alive.

The expected raw hero is null during death or unreadable occlusion. Temporal
tests separately require retaining the previously confirmed hero during death,
confirming a new clear portrait within two observations, and returning to alive
on respawn. Labels are not generated from recognizer predictions. These frames
cover regressions from one replay, not a general accuracy benchmark.
