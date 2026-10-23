#!/usr/bin/env python3
"""Packaging contract tests; no GUI, user profile, network, or target toolchain."""
import json
import contextlib
import io
import os
import struct
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path
import tempfile
import unittest
import zipfile

from package_plugin_runtime import (LAYOUTS, MANIFEST, check_links, copy_runtime, inventory, verify,
                                    elf_dynamic, origin_rpath, clean_environment, fresh_output, stage,
                                    macho_architectures, macos_check_architectures)
from package_plugin_wheelhouse import pins, wheel_metadata, check_import, package, main as wheelhouse_main
from test_plugin_runtime_ci import run_check, archive_roundtrip


class DistributionContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_host_layouts(self):
        # Match find_bundled_python_home and bundled_uv_path, not development fallbacks.
        self.assertEqual(LAYOUTS["macos"]["python"], "Contents/Frameworks/python")
        self.assertEqual(LAYOUTS["windows"]["python"], "python")
        linux = LAYOUTS["linux"]
        self.assertEqual(Path(linux["resources"]).parent / "lib/python", Path(linux["python"]))
        for layout in LAYOUTS.values():
            uv = Path(layout["uv"])
            self.assertIn(uv.parent, [Path(layout["host"]) / "tools/uv", Path(layout["resources"]) / "tools/uv"])

    def test_inventory_rejects_modified_missing_and_extra_files(self):
        payload = self.root / "payload"
        payload.mkdir()
        path = payload / "file"
        path.write_text("original")
        manifest = {"schema": 1, "platform": "macos", "layout": LAYOUTS["macos"], "files": inventory(payload)}
        (payload / MANIFEST).write_text(json.dumps(manifest))
        self.assertEqual(verify(payload)["integrity"], "passed")
        path.write_text("modified")
        with self.assertRaisesRegex(ValueError, "changed"):
            verify(payload)
        path.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(payload)
        path.write_text("original")
        (payload / "extra").write_text("extra")
        with self.assertRaisesRegex(ValueError, "extra"):
            verify(payload)

    def test_external_symlink_rejected(self):
        source = self.root / "source"
        source.mkdir()
        (self.root / "outside").write_text("external")
        (source / "link").symlink_to(self.root / "outside")
        with self.assertRaisesRegex(ValueError, "External"):
            check_links(source)

    def test_internal_absolute_symlink_becomes_relocatable(self):
        source = self.root / "source"
        source.mkdir()
        (source / "target").write_text("inside")
        (source / "link").symlink_to(source / "target")
        copy_runtime(source, self.root / "copy")
        (self.root / "copy").rename(self.root / "moved")
        self.assertEqual((self.root / "moved/link").read_text(), "inside")
        check_links(self.root / "moved")

    def test_nested_output_rejected_before_input_mutation_or_execution(self):
        source = self.root / "input"
        source.mkdir()
        (source / "sentinel").write_text("preserve")
        alias = self.root / "alias"
        alias.symlink_to(source, target_is_directory=True)
        before = inventory(source)
        for output in [source / "new" / "output", alias / "new" / "output"]:
            with self.assertRaisesRegex(ValueError, "outside every input"):
                # Missing uv would fail later; output rejection must happen first.
                stage(SimpleNamespace(python_root=source, output=output))
            with patch("package_plugin_wheelhouse.verify", side_effect=AssertionError("native execution")):
                with self.assertRaisesRegex(ValueError, "outside every input"):
                    package(SimpleNamespace(runtime=source, wheelhouse=self.root, output=output))
            self.assertEqual(inventory(source), before)
        wheels = self.root / "wheels"
        wheels.mkdir()
        with self.assertRaisesRegex(ValueError, "outside every input"):
            package(SimpleNamespace(runtime=source, wheelhouse=wheels, output=wheels / "output"))
        self.assertEqual(list(wheels.iterdir()), [])

    def test_broken_output_symlink_is_not_a_fresh_artifact(self):
        output = self.root / "output"
        output.symlink_to(self.root / "missing")
        with self.assertRaisesRegex(ValueError, "already exists"):
            fresh_output(output)
        self.assertTrue(output.is_symlink())

    def test_directory_symlink_remains_a_directory_after_move(self):
        source = self.root / "source"
        (source / "directory").mkdir(parents=True)
        (source / "directory/file").write_text("retained")
        (source / "alias").symlink_to(source / "directory", target_is_directory=True)
        copy_runtime(source, self.root / "copy")
        (self.root / "copy").rename(self.root / "moved")
        self.assertTrue((self.root / "moved/alias").is_dir())
        self.assertEqual((self.root / "moved/alias/file").read_text(), "retained")
        self.assertFalse(Path(os.readlink(self.root / "moved/alias")).is_absolute())

    @staticmethod
    def fat_macho(cpus, endian=">", entry_size=20):
        magic = 0xcafebabe if entry_size == 20 else 0xcafebabf
        return struct.pack(endian + "II", magic, len(cpus)) + b"".join(
            struct.pack(endian + "I", cpu) + bytes(entry_size - 4) for cpu in cpus)

    def test_macho_architecture_headers_and_truncation(self):
        path = self.root / "binary"
        arm, intel = 0x0100000c, 0x01000007
        for endian in ["<", ">"]:
            for magic in [0xfeedface, 0xfeedfacf]:
                path.write_bytes(struct.pack(endian + "II", magic, arm))
                self.assertEqual(macho_architectures(path), {arm})
            for entry_size in [20, 32]:
                path.write_bytes(self.fat_macho([arm, intel], endian, entry_size))
                self.assertEqual(macho_architectures(path), {arm, intel})
                path.write_bytes(path.read_bytes()[:-1])
                with self.assertRaisesRegex(ValueError, "Malformed"):
                    macho_architectures(path)
        path.write_bytes(b"\xcf\xfa\xed\xfe")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            macho_architectures(path)

    def test_universal_host_rejects_single_architecture_runtime(self):
        layout = LAYOUTS["macos"]
        python = self.root / layout["python"] / layout["executable"]
        python.parent.mkdir(parents=True)
        arm, intel = 0x0100000c, 0x01000007
        python.write_bytes(struct.pack("<II", 0xfeedfacf, arm))
        self.assertEqual(macos_check_architectures(self.root, layout)["required"], ["arm64"])
        host = self.root / layout["host"] / "host"
        host.parent.mkdir(parents=True)
        host.write_bytes(self.fat_macho([arm, intel]))
        with self.assertRaisesRegex(ValueError, "do not cover"):
            macos_check_architectures(self.root, layout, host.relative_to(self.root).as_posix())
        python.write_bytes(self.fat_macho([arm, intel]))
        self.assertEqual(macos_check_architectures(self.root, layout, host.relative_to(self.root).as_posix())["required"],
                         ["x86_64", "arm64"])

    def test_ci_reports_real_process_failure_and_timeout(self):
        for name, command, options in [
            ("failed", [sys.executable, "-c", "raise SystemExit(7)"], {}),
            ("timeout", [sys.executable, "-c", "import time; time.sleep(10)"], {"timeout": 0.1}),
            ("missing", [str(self.root / "missing-command")], {}),
            ("wrong-rejection", [sys.executable, "-c", "raise SystemExit(1)"], {"expected": 1, "diagnostic": "specific cause"}),
        ]:
            checks = []
            with self.assertRaisesRegex(RuntimeError, "failed; see results"):
                run_check(self.root, checks, name, command, **options)
            report = json.loads((self.root / "results.json").read_text())
            self.assertFalse(report[-1]["passed"])
            self.assertEqual(report[-1]["name"], name)

    def test_archive_preserves_runtime_integrity_after_extraction(self):
        payload = self.root / "runtime"
        payload.mkdir()
        (payload / "tool").write_text("payload")
        (payload / "tool").chmod(0o755)
        (payload / "alias").symlink_to("tool")
        manifest = {"schema": 1, "platform": "macos", "layout": LAYOUTS["macos"], "files": inventory(payload)}
        (payload / MANIFEST).write_text(json.dumps(manifest))
        offline = self.root / "offline"
        offline.mkdir()
        (offline / "lock").write_text("locked")
        unpacked = archive_roundtrip(self.root, [], payload, offline)
        self.assertEqual(verify(unpacked / "runtime")["integrity"], "passed")
        self.assertEqual((unpacked / "offline-numpy/lock").read_text(), "locked")

    def test_offline_pins_reject_options_urls_and_unpinned_input(self):
        path = self.root / "requirements.in"
        for invalid in ["--index-url https://example.invalid", "thing @ https://example.invalid/a.whl", "thing>=1.0"]:
            path.write_text(invalid)
            with self.assertRaises(ValueError):
                pins(path)
        path.write_text("numpy==2.2.6\nthing[extra]==1.0 # comment\n")
        self.assertEqual(pins(path), ["numpy==2.2.6", "thing[extra]==1.0"])

    def test_elf_program_headers_distinguish_static_tools(self):
        path = self.root / "tool"
        for bits, endian in [(32, "<"), (32, ">"), (64, "<"), (64, ">")]:
            header_size, entry_size = (64, 56) if bits == 64 else (52, 32)
            for dynamic in (False, True):
                data = bytearray(header_size + entry_size)
                data[:6] = b"\x7fELF" + bytes([2 if bits == 64 else 1, 1 if endian == "<" else 2])
                struct.pack_into(endian + ("Q" if bits == 64 else "I"), data, 32 if bits == 64 else 28, header_size)
                struct.pack_into(endian + "HH", data, 54 if bits == 64 else 42, entry_size, 1)
                struct.pack_into(endian + "I", data, header_size, 2 if dynamic else 1)
                path.write_bytes(data)
                self.assertEqual(elf_dynamic(path), dynamic)
        path.write_bytes(b"\x7fELF")
        with self.assertRaisesRegex(ValueError, "Malformed"):
            elf_dynamic(path)

    def test_elf_rpath_must_resolve_inside_runtime(self):
        binary = self.root / "bin/python"
        self.assertEqual(origin_rpath(binary, self.root, "$ORIGIN/../lib"), "$ORIGIN/../lib")
        for value in ["/developer/lib", "$ORIGIN/../../outside", "$ORIGINevil", "${ORIGIN}/../../outside"]:
            with self.assertRaises(ValueError):
                origin_rpath(binary, self.root, value)

    def test_clean_environment_rejects_preload_and_user_python(self):
        with patch.dict(os.environ, {"LD_PRELOAD": "/injected.so", "PYTHONPATH": "/user", "UV_CACHE_DIR": "/cache"}):
            environment = clean_environment()
        self.assertNotIn("LD_PRELOAD", environment)
        self.assertNotIn("PYTHONPATH", environment)
        self.assertNotIn("UV_CACHE_DIR", environment)

    def test_checked_import_must_come_from_install_target(self):
        (self.root / "package_contract.py").write_text("VALUE = 7\n")
        check_import(Path(sys.executable), self.root, "package_contract")
        with self.assertRaisesRegex(ValueError, "Import did not come from installed wheelhouse"):
            check_import(Path(sys.executable), self.root, "json")

    def test_wheel_archive_traversal_is_rejected_before_install(self):
        path = self.root / "bad.whl"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("../outside", "escape")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            wheel_metadata(path)

    def test_offline_timeout_has_a_bounded_actionable_cli_error(self):
        diagnostic = io.StringIO()
        with patch.object(sys, "argv", ["wheelhouse", "--runtime", str(self.root), "--verify-package", str(self.root)]), \
             patch("package_plugin_wheelhouse.verify_package",
                   side_effect=subprocess.TimeoutExpired(["uv", "private argument"], 120)), \
             contextlib.redirect_stderr(diagnostic):
            with self.assertRaises(SystemExit) as result:
                wheelhouse_main()
        self.assertEqual(result.exception.code, 1)
        self.assertIn("120 second time limit", diagnostic.getvalue())
        self.assertNotIn("private argument", diagnostic.getvalue())
        self.assertNotIn("Traceback", diagnostic.getvalue())


if __name__ == "__main__":
    unittest.main()
