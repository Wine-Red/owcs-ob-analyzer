from pathlib import Path

from PyInstaller.utils.hooks import collect_data_files, collect_submodules


SPEC_DIR = Path(SPECPATH)
PROJECT_ROOT = SPEC_DIR.parents[1]
SOURCE_ROOT = PROJECT_ROOT / "src"

datas = collect_data_files("rapidocr_onnxruntime")
datas.append(
    (
        str(SOURCE_ROOT / "owcs_ob_analyzer" / "resources"),
        "owcs_ob_analyzer/resources",
    )
)

hiddenimports = collect_submodules("rapidocr_onnxruntime")
hiddenimports.extend(
    [
        "dxcam",
        "mss.windows",
        "onnxruntime.capi._pybind_state",
    ]
)

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
