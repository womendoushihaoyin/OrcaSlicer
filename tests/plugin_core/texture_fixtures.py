"""Fixed ResourceTexture contract fixtures, generated without image or network dependencies."""
import struct
import zlib


def png(width, height, pixels, color=False):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    stride = width * (3 if color else 1)
    scanlines = b"".join(b"\0" + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2 if color else 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


CODE = '''import orca, pathlib
class Texture(orca.resource_texture.ResourceTexturePluginCapabilityBase):
    def get_name(self): return "PACK_NAME"
    def get_texture_dir(self):
        self.calls = getattr(self, "calls", 0) + 1
        if self.calls != 1: raise RuntimeError("get_texture_dir must only run at materialization")
        return str(pathlib.Path(__file__).parent / "textures")
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Texture)
'''


def prepare(root, wheel):
    """Append packages to an existing fixture root; `wheel` is fixtures.wheel."""
    gray = png(2, 2, [0, 64, 192, 255])
    white = png(2, 2, [255] * 4)
    rgb = png(2, 2, [255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255], color=True)
    (root / "texture-gray.png").write_bytes(gray)
    (root / "texture-white.png").write_bytes(white)
    (root / "texture-rgb.png").write_bytes(rgb)
    for key, display, image in [("texturepacka", "Contract Pack A", gray), ("texturepackb", "Contract Pack B", white)]:
        wheel(root / f"{key}-1.0.0-py3-none-any.whl", key, CODE.replace("PACK_NAME", display),
              extra={f"{key}/textures/same.png": image, f"{key}/textures/readme.txt": b"ignored"})
    bad = CODE.replace("PACK_NAME", "Invalid Pack").replace('/ "textures"', '/ "missing-textures"')
    wheel(root / "textureinvalid-1.0.0-py3-none-any.whl", "textureinvalid", bad)
