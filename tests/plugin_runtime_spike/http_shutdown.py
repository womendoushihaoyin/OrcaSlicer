"""Exercise the real Http.cpp against an ephemeral local TLS streaming server."""
import http.server
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading

stop = threading.Event()
paths = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        paths.append(self.path)
        self.send_response(200)
        self.send_header("Content-Length", "300" if self.path == "/slow" else "2")
        self.end_headers()
        try:
            if self.path == "/slow":
                for _ in range(300):
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    if stop.wait(0.1):
                        return
            else:
                self.wfile.write(b"ok")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def do_PUT(self):
        paths.append(self.path)
        self.send_error(503)

    def log_message(self, *_):
        pass


with tempfile.TemporaryDirectory(prefix="orca-http-shutdown-") as directory:
    root = Path(directory)
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem"),
                    "-subj", "/CN=localhost", "-days", "1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root / "cert.pem", root / "key.pem")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        subprocess.run([sys.argv[1], f"https://127.0.0.1:{server.server_port}", str(root / "cert.pem")], check=True, timeout=40)
        assert paths.count("/ok") == 2 and paths.count("/slow") == 6, paths
        assert "/late" not in paths and "/late-put" not in paths, "Shutdown accepted a new request"
    finally:
        stop.set()
        server.shutdown()
        thread.join()
        server.server_close()
