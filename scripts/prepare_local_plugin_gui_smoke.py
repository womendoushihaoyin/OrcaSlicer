#!/usr/bin/env python3
"""Prepare an isolated macOS development app for native GUI smoke tests.

Does not launch the app, modify an installed app, or register file associations.
The caller launches the printed bundle ID through their GUI automation tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--python-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("install", "restart", "disabled", "manager", "pages", "pages_multi", "pages_multi_restart", "dock_write", "dock_restart", "ui", "host", "config", "replacement", "slicing", "preset", "workflow", "texture", "dualhost", "legacy_states"), required=True)
    parser.add_argument("--uv-executable", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--texture-color", action="store_true", help="Require actual color painting in texture GUI smoke")
    parser.add_argument("--texture-authoring", action="store_true", help="Save editable seam/UV authoring, reopen 3MF, then bake in a second dialog")
    parser.add_argument("--texture-image-adjust", action="store_true", help="Require Save as copy to change plugin pixels in the isolated texture library")
    parser.add_argument("--texture-mix", action="store_true", help="Two real editor dialogs: Preview/Cancel, then Preview/Bake with automatic mixed recipes")
    args = parser.parse_args()
    if args.texture_mix and (args.mode != "texture" or args.texture_authoring or args.texture_image_adjust):
        parser.error("--texture-mix requires --mode texture and cannot combine authoring/image-adjust modes")
    if args.texture_mix:
        args.texture_color = True
    if args.texture_color and args.mode != "texture":
        parser.error("--texture-color requires --mode texture")
    if args.texture_authoring and args.mode != "texture":
        parser.error("--texture-authoring requires --mode texture")
    if args.texture_image_adjust and args.mode != "texture":
        parser.error("--texture-image-adjust requires --mode texture")
    if sys.platform != "darwin":
        parser.error("This helper only prepares macOS development bundles")
    repo = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    if args.mode == "legacy_states":
        if (output / "data_dir").exists():
            parser.error("legacy_states requires a fresh profile; choose a new output directory")
        if not args.fixture:
            parser.error("legacy_states requires the unchanged canonical multi wheel via --fixture")
        expected = {
            "orca_dualhost_multi-1.0.0-py3-none-any.whl": "a1349e4d4493ed38bad8a7198c1fbecd09d4f868e65e7daf4494ee7da4da575f",
            "reference-cube.stl": "8f6176f7f70c5aa56a656437194b6869a58593e297954c381c85c9fcde7530bf",
        }
        if args.fixture.name != "orca_dualhost_multi-1.0.0-py3-none-any.whl":
            parser.error("legacy_states requires the fixed multi-capability package")
        for name, digest in expected.items():
            candidate = args.fixture.parent / name
            if not candidate.is_file() or hashlib.sha256(candidate.read_bytes()).hexdigest() != digest:
                parser.error("Canonical legacy input changed or missing: " + name)
    output.mkdir(parents=True, exist_ok=True)
    run_id = hashlib.sha256(str(output).encode()).hexdigest()[:10]
    bundle_id = "com.snapmaker.codex.local-plugin-smoke.run" + run_id + "." + args.mode
    app = output / ("LocalPluginSmoke-" + args.mode + ".app")
    if app.exists():
        parser.error("App already exists; choose a new output directory instead of replacing a potentially running app")
    executable_dir = app / "Contents/MacOS"
    executable_dir.mkdir(parents=True)
    executable = executable_dir / "Snapmaker_Orca"
    python_root = args.python_root.resolve(strict=True)
    # Sign the standalone development executable before placing it in an app.
    # Signing a path inside an assembled app makes codesign validate all nested
    # Python tools as bundle components. Distribution signing is a separate gate.
    with tempfile.TemporaryDirectory(prefix=".smoke-sign-", dir=output) as staging:
        staged_executable = Path(staging) / "Snapmaker_Orca"
        shutil.copy2(args.binary.resolve(strict=True), staged_executable)
        subprocess.run(["install_name_tool", "-change", str(python_root / "lib/libpython3.12.dylib"),
                        "@executable_path/../Frameworks/python/lib/libpython3.12.dylib", str(staged_executable)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", "--identifier", bundle_id, str(staged_executable)], check=True)
        shutil.copy2(staged_executable, executable)
    bundled_python = app / "Contents/Frameworks/python"
    shutil.copytree(python_root, bundled_python, symlinks=True)
    if args.uv_executable:
        bundled_uv = executable_dir / "tools/uv/uv"
        bundled_uv.parent.mkdir(parents=True)
        shutil.copy2(args.uv_executable.resolve(strict=True), bundled_uv)
    (app / "Contents/Resources").symlink_to(repo / "resources", target_is_directory=True)
    report = output / ("gui-" + args.mode + ".json")
    # The launched app must not need access to the developer's Documents
    # directory merely to read a test input. Stage the exact fixtures alongside
    # the isolated profile before launch (also keeps the run reproducible).
    source_fixture = (args.fixture or repo / "docs/plugins/examples/native_host_hello.py").resolve(strict=True)
    fixture = output / "fixtures" / source_fixture.name
    fixture.parent.mkdir(exist_ok=True)
    shutil.copy2(source_fixture, fixture)
    if args.mode == "legacy_states":
        shutil.copy2(source_fixture.parent / "reference-cube.stl", fixture.parent / "reference-cube.stl")
    if args.mode == "replacement":
        for variant in ("broken", "updated"):
            target = fixture.parent / variant / fixture.name
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(source_fixture.parent / variant / source_fixture.name, target)
    # GUI_App::init_app_config also discovers this portable directory when macOS
    # reopens an app without the original command-line arguments.
    profile = output / "data_dir"
    profile.mkdir(exist_ok=True)
    config = profile / "Snapmaker_Orca.conf"
    if not config.exists():
        # Keep the normal setup wizard dismissible in an isolated test profile.
        # Privacy consent stays false; no account or user profile is imported.
        config.write_text(json.dumps({"app": {"privacy_policy_isagree": False,
                                              "show_splash_screen": False,
                                              "sync_system_preset": False,
                                              "sync_user_preset": False,
                                              "single_instance": False},
                                      "firstguide": {"finish": True}}, indent=2) + "\n")
    info = {
        "CFBundleExecutable": "Snapmaker_Orca",
        "CFBundleIdentifier": bundle_id,
        "CFBundleName": "Local Plugin Smoke " + args.mode,
        "CFBundlePackageType": "APPL",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "0.1",
        "NSHighResolutionCapable": True,
        "LSEnvironment": {
            "ORCA_LOCAL_PLUGIN_SMOKE_FILE": str(fixture),
            "ORCA_LOCAL_PLUGIN_SMOKE_REPORT": str(report),
            "ORCA_LOCAL_PLUGIN_SMOKE_MODE": args.mode,
            "ORCA_LOCAL_PLUGIN_SMOKE_DATADIR": str(profile),
            "ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_COLOR": "1" if args.texture_color else "0",
            "ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_MIX": "1" if args.texture_mix else "0",
            "ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_AUTHORING": "1" if args.texture_authoring else "0",
            "ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_IMAGE_ADJUST": "1" if args.texture_image_adjust else "0",
        },
    }
    with (app / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump(info, stream)
    subprocess.run([
        "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
        "-f", str(app),
    ], check=True)
    print(json.dumps({"bundle_id": bundle_id, "additional_arguments": ["--datadir", str(profile)],
                      "creates_new_application_instance": True}, ensure_ascii=False))
    print("Expected report:", report)


if __name__ == "__main__":
    main()
