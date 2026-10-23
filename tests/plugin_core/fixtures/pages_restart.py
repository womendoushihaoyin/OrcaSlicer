# /// script
# requires-python = ">=3.12"
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Isolated Multi Pages Restart Contract"
# version = "1.0.0"
# ///
"""GUI fixture: independent automatic bridges and persisted per-page sessions."""
import json
import uuid
import orca

SESSION = uuid.uuid4().hex


class PageBase(orca.pages.PagesPluginCapabilityBase):
    label = ""

    def get_name(self):
        return self.label

    def get_icon(self):
        self._icon_called = True
        return "printer.svg" if self.label.endswith("Alpha") else "phase1-missing-icon-expected.svg"

    def get_ui(self):
        identity = json.dumps({"page": self.label, "session": SESSION})
        return '''<!doctype html><html><head><meta charset="utf-8"></head>
<body><h1>''' + self.label + '''</h1><p id="status">Connecting</p><script>
const identity = ''' + identity + ''';
function connect() {
 if (!window.orca) { setTimeout(connect, 25); return; }
 orca.onMessage(function(data) {
   if (data.page !== identity.page || data.session !== identity.session) {
     document.getElementById('status').textContent = 'Wrong page or session'; return;
   }
   document.getElementById('status').textContent = identity.page + ' roundtrip passed';
   orca.postMessage({kind:'ack', page:data.page, session:data.session});
 });
 orca.postMessage({kind:'hello', page:identity.page, session:identity.session});
}
connect();</script></body></html>'''

    def on_message(self, message):
        if message.get("page") != self.label or message.get("session") != SESSION:
            raise RuntimeError("Page received another capability's message or a stale generation")
        if message.get("kind") == "hello":
            self.post_message({"page": self.label, "session": SESSION})
        elif message.get("kind") == "ack":
            config = json.loads(self.get_config())
            sessions = config.setdefault("sessions", [])
            if SESSION not in sessions:
                sessions.append(SESSION)
            config.update(page=self.label, current_session=SESSION, roundtrip=True,
                          icon_callback=getattr(self, "_icon_called", False))
            if not self.save_config(json.dumps(config)):
                raise RuntimeError("Cannot persist page roundtrip")


class Alpha(PageBase):
    label = "Phase1 Page Alpha"


class Beta(PageBase):
    label = "Phase1 Page Beta"


@orca.plugin
class Package(orca.base):
    def register_capabilities(self):
        orca.register_capability(Alpha)
        orca.register_capability(Beta)
