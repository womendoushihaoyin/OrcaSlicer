# /// script
# requires-python = ">=3.12"
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Isolated Dock Restart Contract"
# version = "1.0.0"
# ///
"""Real plugin UI handles retained until the normal host window closes."""
import orca

TITLE = "Restart layout α | ; = \\"
PANELS = []


class Docks(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Dock restart"

    def execute(self):
        for index in range(2):
            PANELS.append(orca.host.ui.create_dock_panel(
                f"<html><body><h1>Restart pane {index + 1}</h1></body></html>",
                title=TITLE, width=240, height=180, dock="right",
                on_message=lambda message: None,
                on_close=lambda: None,
            ))
        return orca.ExecutionResult.success("Two dock panels requested", "")


@orca.plugin
class Package(orca.base):
    def register_capabilities(self):
        orca.register_capability(Docks)
