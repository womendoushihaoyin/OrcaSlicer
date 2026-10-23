#!/usr/bin/env python3
"""Offline SDK verifier contracts; fixture names are not a published SDK."""
import ast
import json
from pathlib import Path
import tempfile
import sys
import types
from unittest.mock import patch
import unittest

from package_plugin_runtime import digest
from export_plugin_sdk import signatures, export
from verify_plugin_sdk import verify_members, verify_sdk


class SdkVerificationTests(unittest.TestCase):
    def test_pybind_dispatcher_does_not_hide_typed_overloads(self):
        def fixture():
            pass
        fixture.__doc__ = "rotate(*args, **kwargs)\nOverloaded function.\n1. rotate(self: object, x: int) -> None\n2. rotate(self: object, x: str) -> None"
        self.assertEqual(signatures("rotate", fixture), ["rotate(self: object, x: int) -> None", "rotate(self: object, x: str) -> None"])

    def test_live_export_keeps_arithmetic_protocol_methods(self):
        module = types.ModuleType("orca")
        class Point:
            def __add__(self, other): return self
            def __sub__(self, other): return self
            def __mul__(self, scalar): return self
            def __repr__(self): return "Point fixture"
        Point.__module__ = "orca"
        module.Point = Point
        with tempfile.TemporaryDirectory() as temporary, patch.dict(sys.modules, {"orca": module}):
            root = Path(temporary) / "sdk"
            export(root)
            api = json.loads((root / "api.json").read_text())
            point = next(item for item in api["modules"]["orca"] if item["name"] == "Point")
            names = {item["name"] for item in point["members"]}
            self.assertTrue({"__add__", "__sub__", "__mul__", "__repr__"} <= names)
            self.assertNotIn("__dict__", names)
            verify_members(ast.parse((root / "typings/orca/__init__.pyi").read_text()).body,
                           api["modules"]["orca"], "orca")

    def test_overloads_and_nested_classes_preserve_inventory(self):
        tree = ast.parse("class Fixture:\n def method(self, x: int): ...\n def method(self, x: str): ...\n")
        verify_members(tree.body, [{"name": "Fixture", "members": [{"name": "method"}]}], "fixture")

    def test_missing_nested_member_is_rejected(self):
        tree = ast.parse("class Fixture: ...")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            verify_members(tree.body, [{"name": "Fixture", "members": [{"name": "method"}]}], "fixture")

    def test_extra_member_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            verify_members(ast.parse("invented: int").body, [], "fixture")

    def fixture(self, root):
        stub = root / "typings/orca/__init__.pyi"
        stub.parent.mkdir(parents=True)
        stub.write_text("fixture: int\n", encoding="utf-8")
        (root / "api.json").write_text(json.dumps({"modules": {"orca": [{"name": "fixture"}]}}), encoding="utf-8")
        manifest = {"schema": 1, "source": "live-embedded-orca", "files": {
            path.relative_to(root).as_posix(): digest(path) for path in root.rglob("*") if path.is_file()}}
        (root / "sdk.json").write_text(json.dumps(manifest), encoding="utf-8")
        return stub

    def test_file_tamper_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stub = self.fixture(root)
            self.assertEqual(verify_sdk(root)["live_members"], "matched")
            stub.write_text("invented: int\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "integrity mismatch"):
                verify_sdk(root)

    def test_nested_manifest_is_an_extra_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.fixture(root)
            extra = root / "examples/sdk.json"
            extra.parent.mkdir()
            extra.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "integrity mismatch"):
                verify_sdk(root)


if __name__ == "__main__":
    unittest.main()
