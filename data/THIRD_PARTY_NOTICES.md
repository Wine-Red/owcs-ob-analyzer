# Third-party notices and model provenance

Native plugin source: GPL-3.0-or-later; full license is supplied in LICENSE. OBS Studio is provided by its own installation and is not included in the plugin archive. Qt is dynamically supplied by OBS; no Qt DLLs are redistributed here.

- OpenCV 4.10.0: Apache-2.0. Bundled world DLL is renamed `owcs-opencv4100.dll` to isolate it from other plugins. License texts are in licenses/. No FFmpeg OpenCV video I/O plugin is bundled.
- ONNX Runtime 1.20.1: MIT, Microsoft Corporation. CPU runtime is renamed `owcs-onnxruntime.dll`; its MIT license and third-party notices are included. No GPU runtime is required.
- PP-OCRv4 Chinese/Latin recognition model: distributed via RapidOCR/PaddleOCR (Apache-2.0), https://github.com/RapidAI/RapidOCR and https://github.com/PaddlePaddle/PaddleOCR . Apache license text is included. The dictionary is embedded in the ONNX model metadata.
- `ready.png` and numeric glyph features derive from user-provided game screenshots. HUD portrait features derive from official Overwatch hero illustrations. Scoreboard portrait reference features also use DanLane09/Overwatch-Stats-Elo at revision 95d1084f8eae9261735947fd0b02125c3407dd7c. Legacy Dan perk images have been removed from the maintained source assets. No upstream Python executable is shipped or run.

Game artwork and names belong to their respective rights holders, including Blizzard Entertainment. The referenced Dan snapshot did not include a license grant; this file records provenance and does not imply ownership or a permissive relicensing of those visual assets. The code license does not apply to third-party game imagery or reference-derived template data. See models/manifest.json for the per-category sources.

The archive is a technical distribution package, not an official Blizzard or OBS product. Regenerate reference features when the game's UI, hero artwork or perks change.

All 216 current perk mappings and glyphs in resource version 2026.10.16.1 derive from the locally installed Overwatch China client 2.25.0.0.154461N. They were extracted with the MIT-licensed overtools/OWLib DataTool v2.25.0.0+1151; English names were read from the same build's CDN metadata. The extraction tool is not redistributed. The source manifest records per-image SHA256, hero/loadout/texture/current-perk GUIDs and Chinese names. Full build/tool provenance and bounded validation coverage are in assets/recognition/client-perks-provenance.json. Blizzard retains the game artwork rights. Legacy website perk glyphs have been removed from the maintained source assets.

The source package also includes the original reference artwork under assets/recognition with explicit mappings and provenance. Unselected HUD/scoreboard placeholder references derive from user-provided screenshots dated 2026-10-13. The unselected regression canvases reconstruct full-frame placement from those cropped screenshots; they are not independent full-frame captures. Copyright in the underlying game artwork remains with Blizzard Entertainment.
