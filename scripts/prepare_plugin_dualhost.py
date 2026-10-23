#!/usr/bin/env python3
"""Build deterministic installable single/multi-capability wheels; never run a host."""
import argparse
import ast
import base64
import csv
import hashlib
import io
import json
from pathlib import Path
import zipfile
import struct

UPSTREAM = "14a1aa333b6da3ecadf853a4a6285b0f193b7605"
CAPABILITIES = ("script", "pages", "slicing", "texture")
OPERATIONS = {
    "fresh-load": ["install", "enable", "load", "exercise", "close"],
    "installed-disabled": ["install", "disable", "restart", "verify_disabled", "close"],
    "disable-reenable": ["install", "enable", "load", "exercise", "disable", "verify_disabled", "enable", "exercise", "close"],
    "config-restart": ["install", "enable", "load", "set_config", "exercise", "close", "restart", "verify_config", "exercise", "close"],
    "same-package-reload": ["install", "enable", "load", "exercise", "reload", "exercise", "close"],
    "uninstall-reinstall": ["install", "enable", "load", "exercise", "uninstall", "verify_uninstalled", "install", "enable", "load", "exercise", "close"],
    "error-recovery": ["install", "enable", "load", "set_error", "exercise_error", "recover", "exercise", "close"],
}
FORMS = tuple(OPERATIONS)


def reference_mesh():
    vertices = [(0, 0, 0), (20, 0, 0), (20, 20, 0), (0, 20, 0),
                (0, 0, 1), (20, 0, 1), (20, 20, 1), (0, 20, 1)]
    faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    content = bytearray(b"Orca dual-host 20 x 20 x 1 mm reference cube".ljust(80, b" "))
    content.extend(struct.pack("<I", len(faces)))
    for face in faces:
        values = [0., 0., 0.] + [float(value) for index in face for value in vertices[index]]
        content.extend(struct.pack("<12fH", *values, 0))
    return bytes(content)


def prepare(output):
    output.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    template = (repo / "tests/plugin_contracts/package.py").read_text(encoding="utf-8")
    image = (repo / "docs/plugins/examples/texture_pack/src/snapmaker_texture_example/textures/height.png").read_bytes()
    manifest = {"schema": 1, "upstream_commit": UPSTREAM, "protocol": "local-dualhost-v1",
                "forms_authority": "Explicit local acceptance protocol; not an upstream documented requirement",
                "forms": list(FORMS), "required_operations": OPERATIONS, "package_format": "wheel",
                "packages": [], "cases": [], "execution": "not-run"}
    for label, capabilities in [(name, [name]) for name in CAPABILITIES] + [("multi", list(CAPABILITIES))]:
        name = "orca_dualhost_" + label
        info = name + "-1.0.0.dist-info"
        code = template.replace('ENABLED_CAPABILITIES = ("script", "pages", "slicing", "texture")',
                                "ENABLED_CAPABILITIES = " + repr(tuple(capabilities)))
        ast.parse(code)
        files = {name + "/__init__.py": code.encode("utf-8"),
                 name + "/textures/height-纹理.png": image,
                 name + "/textures/bad-image.png": b"Deliberately invalid PNG for the resource error/recovery case.\n",
                 info + "/METADATA": ("Metadata-Version: 2.1\nName: " + name + "\nVersion: 1.0.0\nRequires-Python: >=3.12\n\n").encode(),
                 info + "/WHEEL": b"Wheel-Version: 1.0\nGenerator: orca-dualhost-v1\nRoot-Is-Purelib: true\nTag: py3-none-any\n",
                 info + "/top_level.txt": (name + "\n").encode()}
        record = io.StringIO()
        writer = csv.writer(record, lineterminator="\n")
        for filename, data in sorted(files.items()):
            writer.writerow([filename, "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode(), len(data)])
        writer.writerow([info + "/RECORD", "", ""])
        files[info + "/RECORD"] = record.getvalue().encode("utf-8")
        wheel = output / (name + "-1.0.0-py3-none-any.whl")
        with zipfile.ZipFile(wheel, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for filename, data in sorted(files.items()):
                entry = zipfile.ZipInfo(filename, (1980, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, data)
        manifest["packages"].append({"filename": wheel.name, "sha256": hashlib.sha256(wheel.read_bytes()).hexdigest(),
                                      "capabilities": capabilities, "module_sha256": hashlib.sha256(code.encode("utf-8")).hexdigest()})
        for capability in capabilities:
            for form in FORMS:
                manifest["cases"].append({"id": label + "/" + capability + "/" + form,
                    "package": wheel.name, "capability": capability, "form": form,
                    "status": "not-run", "traces": [], "host_observation": None, "gcode": None})
    mesh = reference_mesh()
    (output / "reference-cube.stl").write_bytes(mesh)
    manifest["reference_mesh"] = {"filename": "reference-cube.stl", "sha256": hashlib.sha256(mesh).hexdigest(),
        "size_mm": [20, 20, 1], "triangles": 12}
    manifest["settings_recipe"] = {"scope": "Manually apply the same common settings and retain each host's resolved configuration",
        "layer_height": 0.2, "initial_layer_print_height": 0.2, "wall_loops": 2, "sparse_infill_density": "15%",
        "enable_support": False, "elefant_foot_compensation": 0, "notes": "Use the same printer, nozzle, filament and plate; record resolved configuration artifacts. Engine differences remain visible."}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"packages": len(manifest["packages"]), "cases": len(manifest["cases"]),
                      "execution": "not-run", "output": str(output)}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    prepare(parser.parse_args().output.absolute())
