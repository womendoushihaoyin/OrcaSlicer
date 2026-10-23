#!/usr/bin/env python3
"""Release integration contracts; native application builds are separate checks."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("plugin_release", Path(__file__).parent / "packaging/integrate_plugin_runtime.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
runtime = release.runtime


class ReleaseContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plugin-release-contract-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def pe(machine):
        data = bytearray(70)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, machine)
        return data

    def test_binary_architectures_elf_pe_and_macho(self):
        binary = self.root / "binary"
        for machine, expected in [(0x8664, "x86_64"), (0xaa64, "arm64"), (0x14c, "i386")]:
            binary.write_bytes(self.pe(machine))
            self.assertEqual(release.binary_architectures(binary), {expected})
        for endian in ("<", ">"):
            for machine, expected in [(62, "x86_64"), (183, "arm64")]:
                data = bytearray(64)
                data[:6] = b"\x7fELF\x02" + bytes([1 if endian == "<" else 2])
                struct.pack_into(endian + "H", data, 18, machine)
                binary.write_bytes(data)
                self.assertEqual(release.binary_architectures(binary), {expected})
        binary.write_bytes(struct.pack("<II", 0xfeedfacf, 0x0100000c))
        self.assertEqual(release.binary_architectures(binary), {"arm64"})

    def test_malformed_pe_does_not_pass_architecture_gate(self):
        binary = self.root / "broken.exe"
        binary.write_bytes(b"MZ")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            release.binary_architectures(binary)

    def test_architecture_check_ignores_foreign_templates_not_native_extensions(self):
        python = self.root / "python.exe"
        python.write_bytes(self.pe(0x8664))
        template = self.root / "Lib/site-packages/pip/_vendor/distlib/w32.exe"
        template.parent.mkdir(parents=True)
        template.write_bytes(self.pe(0x14c))
        self.assertEqual(release.check_architectures(self.root, {"x86_64"}, "windows"), 1)
        wrong = self.root / "extension.pyd"
        wrong.write_bytes(self.pe(0x14c))
        with self.assertRaisesRegex(ValueError, "Architecture mismatch"):
            release.check_architectures(self.root, {"x86_64"}, "windows")
        wrong.unlink()
        (self.root / "native").write_bytes(struct.pack("<II", 0xfeedfacf, 0x0100000c))
        self.assertEqual(release.check_architectures(self.root, {"arm64"}, "macos"), 1)

    def test_cache_requires_actual_mode_and_matching_python_and_uv(self):
        cache = self.root / "CMakeCache.txt"
        source = self.root / "python"
        uv = self.root / "uv"
        state = {"python_root": str(source), "uv": str(uv)}
        cache.write_text(f"ORCA_PYTHON_PLUGINS:BOOL=ON\nORCA_PLUGIN_PYTHON_ROOT:PATH={source}\nORCA_PLUGIN_UV_EXECUTABLE:FILEPATH={uv}\n")
        release.check_cache(cache, True, state)
        with self.assertRaisesRegex(ValueError, "mode differs"):
            release.check_cache(cache, False)
        with self.assertRaisesRegex(ValueError, "differs from prepared"):
            release.check_cache(cache, True, {**state, "uv": str(self.root / "other-uv")})
        cache.write_text("ORCA_PYTHON_PLUGINS:BOOL=OFF\n")
        release.check_cache(cache, False)

    def test_dumpbin_resolves_from_cmake_linker_without_developer_path(self):
        tools = self.root / "MSVC Tools with spaces"
        tools.mkdir()
        dumpbin = tools / "dumpbin.exe"
        dumpbin.write_text("inspection tool fixture")
        cache = self.root / "CMakeCache.txt"
        cache.write_text(f"CMAKE_LINKER:FILEPATH={tools / 'link.exe'}\n")
        with patch("shutil.which", return_value=None):
            self.assertEqual(release.windows_dumpbin(cache), str(dumpbin))
            dumpbin.unlink()
            with self.assertRaisesRegex(ValueError, "needs dumpbin"):
                release.windows_dumpbin(cache)

    def make_overlay(self):
        overlay = self.root / "overlay"
        (overlay / "python/Lib").mkdir(parents=True)
        (overlay / "python/Lib/fixture.py").write_text("VALUE = 1\n")
        (overlay / "python312.dll").write_bytes(b"example runtime")
        manifest = {"files": runtime.inventory(overlay)}
        (overlay / runtime.MANIFEST).write_text(json.dumps(manifest))
        return overlay, manifest

    def test_merge_preserves_app_and_rejects_conflicting_dll(self):
        overlay, manifest = self.make_overlay()
        app = self.root / "application"
        app.mkdir()
        (app / "app.exe").write_text("preserved")
        (app / "python312.dll").write_text("unrelated")
        with self.assertRaisesRegex(ValueError, "collides"):
            release.merge_overlay(overlay, app, manifest)
        self.assertEqual((app / "python312.dll").read_text(), "unrelated")
        (app / "python312.dll").unlink()
        release.merge_overlay(overlay, app, manifest)
        release.verify_deployed_files(app, manifest)
        self.assertEqual((app / "app.exe").read_text(), "preserved")
        (app / "python/Lib/fixture.py").write_text("corrupt")
        with self.assertRaisesRegex(ValueError, "changed"):
            release.verify_deployed_files(app, manifest)

    def test_merge_rejects_symlink_parent_before_external_write(self):
        overlay, manifest = self.make_overlay()
        app = self.root / "application"
        outside = self.root / "outside"
        app.mkdir()
        outside.mkdir()
        (app / "python").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "parent is a symlink"):
            release.merge_overlay(overlay, app, manifest)
        self.assertEqual(list(outside.iterdir()), [])

    def test_prepare_cannot_replace_unmanaged_directory(self):
        destination = self.root / "existing"
        destination.mkdir()
        (destination / release.STATE).write_text('{"managed": "other-owner"}')
        candidate = self.root / "candidate"
        candidate.mkdir()
        with self.assertRaisesRegex(ValueError, "unmanaged"):
            release.publish(candidate, destination)
        self.assertTrue(destination.is_dir())
        self.assertTrue(candidate.is_dir())

    @unittest.skipUnless(shutil.which("cpack"), "CPack unavailable; installer input contract is unverified")
    def test_cpack_packages_deployed_tree_not_original_install_rules(self):
        app = self.root / "应用 with spaces"
        (app / "python/Lib").mkdir(parents=True)
        (app / "python/Lib/fixture.py").write_text("runtime included")
        (app / "app.exe").write_text("application included")
        base = self.root / "CPackConfig.cmake"
        base.write_text('set(CPACK_PACKAGE_NAME "PluginFixture")\nset(CPACK_PACKAGE_VERSION "1.0")\n'
                        'set(CPACK_PACKAGE_FILE_NAME "plugin-fixture")\nset(CPACK_PACKAGE_DESCRIPTION "Plugin release contract")\n'
                        'set(CPACK_INSTALL_CMAKE_PROJECTS "nonexistent;wrong;ALL;/")\n')
        config = release.write_cpack_config(base, app)
        result = subprocess.run([shutil.which("cpack"), "--config", str(config), "-G", "TGZ", "-B", str(self.root / "packages")],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.root / "packages/plugin-fixture.tar.gz") as archive:
            contents = {item.name: archive.extractfile(item).read() for item in archive.getmembers() if item.isfile()}
        self.assertTrue(any(name.endswith("python/Lib/fixture.py") and value == b"runtime included" for name, value in contents.items()))
        self.assertTrue(any(name.endswith("app.exe") and value == b"application included" for name, value in contents.items()))


if __name__ == "__main__":
    unittest.main()
