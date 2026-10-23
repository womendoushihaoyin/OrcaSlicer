#!/usr/bin/env python3
"""Opt-in release runtime preparation and transactional application deployment.

Run prepare before configuring, then deploy after assembling the application and
before signing/compressing it. Never starts the GUI or downloads dependencies.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_plugin_runtime as runtime

STATE = "plugin-release.json"
MANAGED = "orca-plugin-release-v1"
ARCH_NAMES = {"aarch64": "arm64", "AMD64": "x86_64", "amd64": "x86_64"}


def architecture(name):
    return ARCH_NAMES.get(name, name)


def binary_architectures(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:4] == b"\x7fELF":
            if len(header) < 20 or header[5] not in (1, 2):
                raise ValueError("Malformed ELF architecture header")
            machine = struct.unpack_from(("<" if header[5] == 1 else ">") + "H", header, 18)[0]
            return {{62: "x86_64", 183: "arm64", 3: "i386"}.get(machine, f"ELF-{machine}")}
        if header[:2] == b"MZ":
            if len(header) != 64:
                raise ValueError("Truncated PE header")
            stream.seek(struct.unpack_from("<I", header, 60)[0])
            pe = stream.read(6)
            if len(pe) != 6 or pe[:4] != b"PE\0\0":
                raise ValueError("Malformed PE architecture header")
            machine = struct.unpack_from("<H", pe, 4)[0]
            return {{0x8664: "x86_64", 0xaa64: "arm64", 0x14c: "i386"}.get(machine, f"PE-{machine}")}
    if runtime.macho(path):
        names = {0x01000007: "x86_64", 0x0100000c: "arm64", 7: "i386", 12: "arm"}
        return {names.get(cpu, f"MachO-{cpu}") for cpu in runtime.macho_architectures(path)}
    return set()


def check_architectures(root, required, platform):
    count = 0
    for path in root.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        target_format = (platform == "macos" and runtime.macho(path) or
                         platform == "linux" and magic == b"\x7fELF" or
                         platform == "windows" and magic[:2] == b"MZ")
        # pip carries launchers for multiple Windows targets as installation data.
        # They are never loaded by this host; actual interpreter/DLL/PYD files are.
        template = "/site-packages/pip/_vendor/distlib/" in path.as_posix() and path.name in {
            "t32.exe", "t64.exe", "t64-arm.exe", "w32.exe", "w64.exe", "w64-arm.exe"}
        if not target_format or template:
            continue
        arches = binary_architectures(path)
        if arches:
            count += 1
            if not required.issubset(arches):
                raise ValueError(f"Architecture mismatch in {path.relative_to(root)}: required {sorted(required)}, found {sorted(arches)}")
    if not count:
        raise ValueError("No native runtime binaries found")
    return count


def cmake_literal(value):
    delimiter = "="
    while "]" + delimiter + "]" in value:
        delimiter += "="
    return "[" + delimiter + "[" + value + "]" + delimiter + "]"


def read_cache(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        match = re.fullmatch(r"([^:]+):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def check_cache(path, enabled, state=None):
    values = read_cache(path)
    if values.get("ORCA_LOCAL_PLUGIN_SPIKE", "OFF").upper() in ("1", "ON", "YES", "TRUE"):
        raise ValueError("Development spike must be OFF for release integration")
    actual = values.get("ORCA_PYTHON_PLUGINS", "OFF").upper() in ("1", "ON", "YES", "TRUE")
    if actual != enabled:
        raise ValueError("CMake cache plugin mode differs from requested release mode; reconfigure without build-only")
    if enabled:
        for key, field in [("ORCA_PLUGIN_PYTHON_ROOT", "python_root"), ("ORCA_PLUGIN_UV_EXECUTABLE", "uv")]:
            if Path(values.get(key, "")).resolve() != Path(state[field]).resolve():
                raise ValueError(f"CMake cache {key} differs from prepared runtime; reconfigure")


def publish(candidate, destination):
    # Only replace our own prepared directory. Application deploy uses a separate
    # explicit transaction below and never removes the source build tree.
    if destination.exists():
        if destination.is_symlink() or json.loads((destination / STATE).read_text(encoding="utf-8")).get("managed") != MANAGED:
            raise ValueError("Refusing to replace an unmanaged release preparation directory")
        shutil.rmtree(destination)
    candidate.rename(destination)


def prepare(args):
    if sys.version_info < (3, 12):
        raise ValueError("Plugin release packaging needs Python 3.12 or newer")
    if args.platform != runtime.native_platform():
        raise ValueError("Release preparation must run on the target OS")
    if args.architecture == "universal":
        raise ValueError("Plugin release integration currently requires a single architecture; use -a arm64 or -a x86_64 and matching Python/uv")
    source = args.python_root.resolve(strict=True)
    uv = args.uv.resolve(strict=True)
    work = args.work_dir.absolute()
    for value in (source, uv.parent):
        if work.resolve().is_relative_to(value):
            raise ValueError("Release preparation directory must be outside its inputs")
    header = source / ("include/Python.h" if args.platform == "windows" else "include/python3.12/Python.h")
    if not header.is_file():
        raise ValueError("Python development header Python.h is missing")
    if args.platform == "windows" and not (source / "libs/python312.lib").is_file():
        raise ValueError("Python embedding import library libs/python312.lib is missing")
    work.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".plugin-prepare-", dir=work.parent) as temporary:
        candidate = Path(temporary) / "prepared"
        candidate.mkdir()
        overlay = candidate / "overlay"
        with contextlib.redirect_stdout(io.StringIO()):
            runtime.stage(SimpleNamespace(python_root=source, uv=uv, output=overlay, platform=args.platform,
                                          uv_version=runtime.UV_VERSION, layout_only=False, host_binary=None))
        verification = runtime.verify(overlay, native=True)
        wanted = architecture(args.architecture or verification["native"]["machine"])
        if architecture(verification["native"]["machine"]) != wanted:
            raise ValueError("Prepared Python executes as a different architecture from the requested build")
        binaries = check_architectures(overlay, {wanted}, args.platform)
        state = {"managed": MANAGED, "platform": args.platform, "architecture": wanted,
                 "python_root": str(source), "uv": str(uv), "runtime_manifest_sha256": runtime.digest(overlay / runtime.MANIFEST),
                 "native_verification": verification, "architecture_binaries": binaries}
        (candidate / STATE).write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        settings = {"ORCA_PYTHON_PLUGINS": ("ON", "BOOL"), "ORCA_LOCAL_PLUGIN_SPIKE": ("OFF", "BOOL"),
                    "ORCA_PLUGIN_PYTHON_ROOT": (source.as_posix(), "PATH"), "ORCA_PLUGIN_UV_EXECUTABLE": (uv.as_posix(), "FILEPATH")}
        (candidate / "plugin-options.cmake").write_text("\n".join(
            f"set({key} {cmake_literal(value)} CACHE {kind} \"Plugin release input\" FORCE)"
            for key, (value, kind) in settings.items()) + "\n", encoding="utf-8")
        publish(candidate, work)
    print(json.dumps({"prepared": str(work), "platform": args.platform, "architecture": wanted}))


def merge_overlay(overlay, destination, manifest):
    # Refuse symlink parents and file collisions rather than following links into
    # the build inputs or silently replacing an unrelated application DLL.
    for relative, entry in manifest["files"].items():
        source = overlay / runtime.checked_relative(relative)
        target = destination / relative
        parent = target.parent
        while parent != destination:
            if parent.is_symlink():
                raise ValueError(f"Application parent is a symlink: {parent.relative_to(destination)}")
            parent = parent.parent
        if target.exists() or target.is_symlink():
            same = ("symlink" in entry and target.is_symlink() and os.readlink(target) == entry["symlink"])
            same = same or ("sha256" in entry and target.is_file() and not target.is_symlink() and runtime.digest(target) == entry["sha256"])
            if not same:
                raise ValueError(f"Runtime file collides with application: {relative}; assemble a clean install tree")
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            target.symlink_to(os.readlink(source), target_is_directory=source.is_dir())
        else:
            shutil.copy2(source, target)
    shutil.copy2(overlay / runtime.MANIFEST, destination / runtime.MANIFEST)


def verify_deployed_files(root, manifest):
    for relative, expected in manifest["files"].items():
        path = root / runtime.checked_relative(relative)
        if "symlink" in expected:
            if not path.is_symlink() or os.readlink(path) != expected["symlink"]:
                raise ValueError(f"Deployed runtime symlink changed: {relative}")
        elif path.is_symlink() or not path.is_file() or runtime.digest(path) != expected["sha256"]:
            raise ValueError(f"Deployed runtime file changed: {relative}")


def windows_dumpbin(cache):
    installed = shutil.which("dumpbin")
    if installed:
        return installed
    # setup-msbuild does not add the VC inspection tools to PATH. CMake has
    # already selected an absolute MSVC linker; its sibling dumpbin matches it.
    linker = Path(read_cache(cache).get("CMAKE_LINKER", ""))
    candidate = linker.with_name("dumpbin.exe") if linker.name else None
    if candidate and candidate.is_absolute() and candidate.is_file():
        return str(candidate)
    raise ValueError("Plugin deployment needs dumpbin from MSVC (PATH or beside CMAKE_LINKER)")


def windows_dependencies(root, hosts, cache):
    dumpbin = windows_dumpbin(cache)
    local = {p.name.lower(): p for p in root.iterdir() if p.is_file()}
    system = Path(os.environ["SystemRoot"]) / "System32"
    pending, seen = list(hosts), set()
    python_linked = False
    while pending:
        target = pending.pop()
        if target in seen:
            continue
        seen.add(target)
        output = runtime.run([dumpbin, "/nologo", "/dependents", target])
        if target in hosts and re.search(r"(?im)^\s*python312\.dll\s*$", output):
            python_linked = True
        for line in output.splitlines():
            name = line.strip()
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.dll", name, re.IGNORECASE):
                continue
            if name.lower() in local:
                pending.append(local[name.lower()])
            elif name.lower().startswith(("api-ms-win-", "ext-ms-win-")) or (system / name).is_file():
                continue
            else:
                raise ValueError(f"Unbundled PE dependency of {target.name}: {name}")
    if not python_linked:
        raise ValueError("Application host does not import the plugin Python DLL")
    return len(seen)


def relocate_hosts(root, hosts, manifest, source, cache):
    layout, platform = manifest["layout"], manifest["platform"]
    if platform == "macos":
        if not any("libpython3.12" in runtime.run(["otool", "-L", host]) for host in hosts):
            raise ValueError("Application host is not linked to the plugin Python runtime")
        # Thin release builds only; dependencies already copied by the app packager.
        runtime.macos_relocate(root, source, layout)
        return runtime.macos_check_links(root, hosts[0])
    if platform == "linux":
        python_linked = False
        for host in hosts:
            needed = runtime.run(["patchelf", "--print-needed", host]).splitlines()
            for name in needed:
                if "libpython3.12" in name:
                    python_linked = True
                    runtime.run(["patchelf", "--replace-needed", name, "libpython3.12.so.1.0", host])
                elif "/" in name:
                    raise ValueError(f"Absolute ELF dependency in host: {name}")
            old = runtime.run(["patchelf", "--print-rpath", host]).split(":")
            paths = ["$ORIGIN/../lib/python/lib", "$ORIGIN/../lib/orca-runtime"]
            paths += [runtime.origin_rpath(host, root, value) for value in old if value.startswith(("$ORIGIN", "${ORIGIN}"))]
            runtime.run(["patchelf", "--set-rpath", ":".join(dict.fromkeys(paths)), host])
        if not python_linked:
            raise ValueError("Application host is not linked to the plugin Python runtime")
        # Audit with the real wrapper's private-library policy and a clean
        # environment. A user's LD_LIBRARY_PATH must not make missing DLLs pass.
        audit = Path(__file__).resolve().parents[1] / "check_appimage_libs.sh"
        for host in hosts:
            runtime.run(["bash", audit, root, host])
        return len(hosts)
    return windows_dependencies(root, hosts, cache)


def deploy(args):
    work = args.work_dir.resolve(strict=True)
    state = json.loads((work / STATE).read_text(encoding="utf-8"))
    if state.get("managed") != MANAGED or state["platform"] != runtime.native_platform():
        raise ValueError("Prepared runtime does not match the native release platform")
    check_cache(args.cache, True, state)
    overlay = work / "overlay"
    runtime.verify(overlay, native=True)
    if runtime.digest(overlay / runtime.MANIFEST) != state["runtime_manifest_sha256"]:
        raise ValueError("Prepared runtime manifest changed")
    manifest = json.loads((overlay / runtime.MANIFEST).read_text(encoding="utf-8"))
    app = args.app_dir.absolute()
    if app.is_symlink() or not app.is_dir() or work.is_relative_to(app.resolve()) or app.resolve().is_relative_to(work):
        raise ValueError("Application must be an independent, assembled directory")
    relative_hosts = [runtime.checked_relative(value) for value in args.host]
    for relative in relative_hosts:
        host = app / relative
        if not host.is_file() or host.is_symlink():
            raise ValueError(f"Application host is missing or a symlink: {relative}")
        if binary_architectures(host) != {state["architecture"]}:
            raise ValueError(f"Application host architecture differs from prepared runtime: {relative}")
    with tempfile.TemporaryDirectory(prefix=".plugin-deploy-", dir=app.parent) as temporary:
        candidate = Path(temporary) / "application"
        shutil.copytree(app, candidate, symlinks=True)
        merge_overlay(overlay, candidate, manifest)
        hosts = [candidate / relative for relative in relative_hosts]
        dependencies = relocate_hosts(candidate, hosts, manifest, Path(state["python_root"]), args.cache)
        verify_deployed_files(candidate, manifest)
        verification = runtime.probe(candidate, {**manifest, "host_binary": relative_hosts[0].as_posix()}, runtime_only=True)
        check_architectures(candidate, {state["architecture"]}, state["platform"])
        result = {"managed": MANAGED, "platform": state["platform"], "architecture": state["architecture"],
                  "runtime": verification, "host_dependencies_checked": dependencies,
                  "host_sha256": {str(path.relative_to(candidate)): runtime.digest(path) for path in hosts},
                  "signing": "pre-distribution-signing", "gui_started": False}
        (candidate / "plugin-deployment.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        backup = Path(temporary) / "previous"
        app.rename(backup)
        try:
            candidate.rename(app)
        except OSError:
            backup.rename(app)
            raise
    print(json.dumps({"deployed": str(app), **result}))


def write_cpack_config(base, app):
    base = base.resolve(strict=True)
    app = app.resolve(strict=True)
    if ";" in app.as_posix():
        raise ValueError("CPack directory lists cannot represent a semicolon in the app path")
    output = base.parent / "PluginCPackConfig.cmake"
    # CPack's default install rules would omit files deployed after cmake install.
    # Package the verified tree instead; preserve NSIS metadata from base config.
    output.write_text(
        "include(" + cmake_literal(base.as_posix()) + ")\n"
        "set(CPACK_INSTALL_CMAKE_PROJECTS \"\")\n"
        "set(CPACK_INSTALLED_DIRECTORIES " + cmake_literal(app.as_posix() + ";/") + ")\n",
        encoding="utf-8")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    pre = sub.add_parser("prepare")
    pre.add_argument("--platform", choices=runtime.LAYOUTS, required=True)
    pre.add_argument("--architecture")
    pre.add_argument("--python-root", type=Path, required=True)
    pre.add_argument("--uv", type=Path, required=True)
    pre.add_argument("--work-dir", type=Path, required=True)
    dep = sub.add_parser("deploy")
    dep.add_argument("--work-dir", type=Path, required=True)
    dep.add_argument("--app-dir", type=Path, required=True)
    dep.add_argument("--cache", type=Path, required=True)
    dep.add_argument("--host", action="append", required=True)
    dep.add_argument("--cpack-config", type=Path)
    chk = sub.add_parser("check-cache")
    chk.add_argument("--cache", type=Path, required=True)
    chk.add_argument("--enabled", choices=("ON", "OFF"), required=True)
    chk.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            prepare(args)
        elif args.command == "deploy":
            deploy(args)
            if args.cpack_config:
                print(json.dumps({"cpack_config": str(write_cpack_config(args.cpack_config, args.app_dir))}))
        else:
            state = json.loads((args.work_dir / STATE).read_text(encoding="utf-8")) if args.work_dir else None
            if args.enabled == "ON" and state is None:
                parser.error("ON cache verification requires --work-dir")
            check_cache(args.cache, args.enabled == "ON", state)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Plugin release integration failed: {error}\n")


if __name__ == "__main__":
    main()
