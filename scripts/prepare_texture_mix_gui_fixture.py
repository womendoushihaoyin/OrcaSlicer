#!/usr/bin/env python3
"""Create the actual ResourceTexture wheel for --texture-mix GUI acceptance."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path

def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    spec.loader.exec_module(module)
    return module

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    fixtures = load(repo / "tests/plugin_core/fixtures.py", "mix_wheel_fixture")
    textures = load(repo / "tests/plugin_core/texture_fixtures.py", "mix_texture_fixture")
    image = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAADAAAAAgCAIAAADbtmxLAAAAA3NCSVQICAjb4U/gAAACsUlEQVRYhb2XW5KrMAxEu4Ufs4K7/3VOsDwfssAQT0yRzOXLlUrIyVFLIvyHAKACFe2qIAAF/YXDebtWhOcXp2+oWPwg/dcBBFjBkMANqPr5BFH8w8Nb/3Yp4otPqR8IcSwCDBl0PdQOuXRAMgLSOdDAUHFKOasyINkMsQLabsQTBEdA6wyojA1FPwTnFvuKakAZrC5Ju/SUQ24GQMsMSMYZis6xqbKSCSBmSLYAaftlZ0PDuDxmQI+xoeTHjczuI4RUNwRtQLubtQPSsaFJly0joE0Md0PW5lIhwBJSqxf16IZzoDslk4Gh0O5nQG6IChYAjsVZ218AGhiSPdSNzFUZUDBDVEDB0kom7bN+rSOgcNPQGchUKRZgAULIEAV6oLUNggnQ1NAwQ8U56n6IRl8hitgyZDR9hnqgYUNNQ/3akDqQpcoMcQjEp10WP5ehrrkOqojQgGwwFqCAawM6G/r+gy7TPdTJ3qwImyEDEjkY2iHCzVC/6rKTIUWkAWXQen4FpTPUA90r2TDUchQDgMgAiEgsRGqro4AC+ro477KhoTdDfTJUEQWhIpkhFHDxktkEKm8buhhqbRlKimCG2owuIP1RBMfZM4zL3cH4omRR3BC9xfb+Wv8qQ/HpYCVLRCDyZsgCtJfsMc/QW6uDR1VEIiLx1VaHt9j2eHuwcjfUr7qsO2S0aMe6GRJA2lNtK9njA6EeEPOVoUTkkMECLl3J9JqhaahfzyE5A2Ui7hlaW4vtI/H7b3bZ8xzyUOdqQLklmgT7P0BHQ8Nt/6lQW9t7yRJEAQHZnmqbof8YattlWZD0oqFhyS5k6FaokwNhBzJD78+hSxmykgly7bvMhpDP6EuG3nwe4rlkici7IXaG1qsZ+pShre2zGRLPkPRbLB0MDZfrnT+Kv01qQa7Igq8ftULSjlsx49kAAAAASUVORK5CYII=")
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    key = "texturemixgradient"
    wheel = root / f"{key}-1.0.0-py3-none-any.whl"
    fixtures.wheel(wheel, key, textures.CODE.replace("PACK_NAME", "Texture mixed gradient"),
                   extra={f"{key}/textures/mixed-gradient.png": image})
    print(json.dumps({"wheel": str(wheel), "png_sha256": hashlib.sha256(image).hexdigest(),
                      "width": 48, "height": 32, "color": True, "nonflat": True}, indent=2))

if __name__ == "__main__":
    main()
