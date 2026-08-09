from pathlib import Path

from PyInstaller.utils.hooks import get_package_paths


SPEC_DIR = Path(SPECPATH)
PROJECT_ROOT = SPEC_DIR.parents[1]
SOURCE_ROOT = PROJECT_ROOT / "src"

_, rapidocr_package = get_package_paths("rapidocr_onnxruntime")
recognition_model = (
    Path(rapidocr_package) / "models" / "ch_PP-OCRv4_rec_infer.onnx"
)
datas = [
    (
        str(recognition_model),
        "owcs_ob_analyzer/resources/ocr",
    )
]
datas.append(
    (
        str(SOURCE_ROOT / "owcs_ob_analyzer" / "resources"),
        "owcs_ob_analyzer/resources",
    )
)

hiddenimports = [
    "dxcam",
    "mss.windows",
    "onnxruntime.capi._pybind_state",
]

a = Analysis(
    [str(SPEC_DIR / "entrypoint.py")],
    pathex=[str(SOURCE_ROOT)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["paddle", "paddleocr", "torch", "tensorflow"],
    noarchive=False,
)
# The application never opens or writes video. OpenCV's standalone FFmpeg
# backend is therefore dead weight, while cv2's image codecs remain available.
a.binaries = [
    entry
    for entry in a.binaries
    if "opencv_videoio_ffmpeg" not in entry[0].casefold()
]
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="OWCSOBAnalyzer",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    contents_directory="_internal",
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="OWCSOBAnalyzer",
)
