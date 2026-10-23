"""Installed unchanged in both real hosts; no standalone/fake orca implementation."""
import hashlib
import json
import os
from pathlib import Path
import threading
import orca

# The deterministic wheel builder changes only this registration list.
ENABLED_CAPABILITIES = ("script", "pages", "slicing", "texture")
_lock = threading.Lock()
_sequence = 0
_source_sha256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def record(kind, event, payload):
    global _sequence
    root = Path(orca.host.plugin.storage())
    root.mkdir(parents=True, exist_ok=True)
    with _lock:
        _sequence += 1
        with (root / "dualhost-events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"schema": 1, "sequence": _sequence, "pid": os.getpid(), "module_sha256": _source_sha256, "capability": kind, "event": event,
                "thread": {"id": threading.get_ident(), "main": threading.current_thread() is threading.main_thread()},
                "payload": payload}, ensure_ascii=False, sort_keys=True) + "\n")


class Trace:
    def get_name(self):
        return {"script": "Dualhost Script", "pages": "Dualhost Page", "slicing": "Dualhost Slice",
                "texture": "Dualhost Texture"}[self.kind]

    def get_default_config(self):
        return {"token": 37, "mode": "success"}

    def configuration(self):
        raw = self.get_config()
        values = self.get_default_config()
        values.update(json.loads(raw))
        record(self.kind, "configuration", {"return_type": type(raw).__name__, "values": values})
        return values

    def on_load(self):
        record(self.kind, "load", {})

    def on_unload(self):
        record(self.kind, "unload", {})

    def on_cancelled(self):
        record(self.kind, "cancelled", {})

    def on_lifecycle_event(self, event, ctx):
        record(self.kind, "lifecycle", {"event": event.name, "code": ctx.code.name, "name": ctx.name,
            "id": ctx.id, "index": ctx.index, "dirty": ctx.dirty, "source": ctx.source})

    def outcome(self, values, data=""):
        mode = values["mode"]
        if mode == "raise":
            record(self.kind, "error", {"type": "RuntimeError", "message": "dualhost deliberate exception"})
            raise RuntimeError("dualhost deliberate exception")
        if mode in ("fatal", "recoverable"):
            status = orca.PluginResult.FatalError if mode == "fatal" else orca.PluginResult.RecoverableError
            result = orca.ExecutionResult.failure(status, "dualhost deliberate failure", data)
        elif mode == "skip":
            result = orca.ExecutionResult.skipped("dualhost deliberate skip")
        else:
            result = orca.ExecutionResult.success("dualhost success", data)
        record(self.kind, "return", {"type": type(result).__name__, "status": result.status.name,
            "message": result.message, "data": result.data,
            "field_types": {"message": type(result.message).__name__, "data": type(result.data).__name__}})
        return result


class Script(Trace, orca.script.ScriptPluginCapabilityBase):
    kind = "script"

    def execute(self):
        values = self.configuration()
        try:
            orca.ExecutionResult.failure("invalid-enum", "invalid argument")
        except TypeError as error:
            # Preserve the actual diagnostic for manual review; compare its class,
            # not pybind11's address/version-dependent formatting.
            record(self.kind, "argument_error", {"type": type(error).__name__, "message": str(error)})
        else:
            raise AssertionError("Invalid PluginResult enum accepted")
        data = {"token": values["token"], "unicode": "中文-α", "null": None, "list": [True, 2.5]}
        return self.outcome(values, json.dumps(data, ensure_ascii=False, sort_keys=True))


class Page(Trace, orca.pages.PagesPluginCapabilityBase):
    kind = "pages"

    def get_ui(self):
        record(self.kind, "page_ui", {"return_type": "str"})
        return '''<!doctype html><meta charset="utf-8"><h1>Dualhost Page</h1>
<button id="ping">Round trip 中文-α</button><p id="result">Waiting</p>
<script>
function ready() {
 if (!window.orca) {setTimeout(ready,25);return;}
 orca.onMessage(data => {
   document.getElementById('result').textContent=JSON.stringify(data);
   orca.postMessage({kind:'ack',payload:data});
 });
 document.getElementById('ping').onclick=()=>orca.postMessage({kind:'ping',value:{text:'中文-α',items:[true,2.5,null]}});
}
ready();
</script>'''

    def on_message(self, message):
        record(self.kind, "page_message", {"type": type(message).__name__, "value": message})
        if message.get("kind") == "ping":
            values = self.configuration()
            if values["mode"] == "raise":
                record(self.kind, "error", {"type": "RuntimeError", "message": "dualhost deliberate page exception"})
                raise RuntimeError("dualhost deliberate page exception")
            self.post_message({"reply": message["value"], "token": values["token"]})
        elif message.get("kind") == "ack":
            record(self.kind, "page_ack", message["payload"])


class Slice(Trace, orca.slicing.SlicingPipelineCapabilityBase):
    kind = "slicing"

    def execute(self, ctx):
        values = self.configuration()
        observation = {"step": ctx.step.name, "has_print": ctx.print is not None,
            "has_object": ctx.object is not None, "cancelled": ctx.cancelled(),
            "wall_loops_type": type(ctx.config_value("wall_loops")).__name__,
            "missing_option": ctx.config_value("dualhost_missing_option"),
            "version_type": type(ctx.orca_version).__name__}
        if ctx.step == orca.slicing.Step.posSlice:
            observation["layers"] = len(ctx.object.layers())
            observation["regions_per_layer"] = [len(layer.regions()) for layer in ctx.object.layers()]
        record(self.kind, "slice", observation)
        if ctx.step == orca.slicing.Step.psGCodePostProcess:
            path = Path(ctx.gcode_path)
            marker = "; ORCA_DUALHOST contract-v1 token=" + str(values["token"])
            with path.open("a", encoding="utf-8") as stream:
                stream.write("\n" + marker + "\n")
            record(self.kind, "gcode", {"marker": marker, "exists": path.is_file(),
                                       "output_name_type": type(ctx.output_name).__name__})
        return self.outcome(values)


class Texture(Trace, orca.resource_texture.ResourceTexturePluginCapabilityBase):
    kind = "texture"

    def get_texture_dir(self):
        root = Path(__file__).parent / "textures"
        # This records the provider contract, not a successful GUI decode/bake.
        files = [{"name": path.name, "bytes": path.stat().st_size,
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                 for path in sorted(root.glob("*.png"))]
        record(self.kind, "resource_provider", {"return_type": "str", "files": files})
        return str(root)


@orca.plugin
class Package(orca.base):
    def register_capabilities(self):
        classes = {"script": Script, "pages": Page, "slicing": Slice, "texture": Texture}
        for kind in ENABLED_CAPABILITIES:
            orca.register_capability(classes[kind])
