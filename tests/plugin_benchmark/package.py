"""Real embedded-orca benchmark package; no per-callback disk IO or fake host."""
import json
from pathlib import Path
import orca

CAPABILITY = "script"  # Replaced by the deterministic fixture builder.

class Trace:
    calls = 0
    steps = None

    def on_load(self):
        self.calls = 0
        self.steps = {}

    def on_unload(self):
        (Path(orca.host.plugin.storage()) / "benchmark-summary.json").write_text(
            json.dumps({"capability": CAPABILITY, "calls": self.calls, "steps": self.steps}, sort_keys=True), encoding="utf-8")

class Script(Trace, orca.script.ScriptPluginCapabilityBase):
    def get_name(self): return "Benchmark Script"
    def execute(self):
        self.calls += 1
        return orca.ExecutionResult.success("", str(self.calls))

class Page(Trace, orca.pages.PagesPluginCapabilityBase):
    def get_name(self): return "Benchmark Page"
    def get_ui(self): return "<!doctype html><meta charset='utf-8'><h1>Benchmark Page</h1>"
    def on_message(self, message):
        self.calls += 1
        self.post_message({"reply": message, "count": self.calls})

class Slice(Trace, orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self): return "Benchmark Slice"
    def execute(self, ctx):
        self.calls += 1
        self.steps[ctx.step.name] = self.steps.get(ctx.step.name, 0) + 1
        return orca.ExecutionResult.success()

class Texture(Trace, orca.resource_texture.ResourceTexturePluginCapabilityBase):
    def get_name(self): return "Benchmark Texture"
    def get_texture_dir(self):
        self.calls += 1
        return str(Path(__file__).parent / "textures")

@orca.plugin
class Package(orca.base):
    def register_capabilities(self):
        orca.register_capability({"script": Script, "pages": Page, "slicing": Slice, "texture": Texture}[CAPABILITY])
