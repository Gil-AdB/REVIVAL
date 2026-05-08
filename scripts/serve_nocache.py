#!/usr/bin/env python3
# Like `python3 -m http.server`, but sends Cache-Control: no-store on
# every response so the browser never reuses a stale DEMO.wasm /
# DEMO.js across rebuilds. Used by `make serve` / `make serve-profile`.
#
# Usage: serve_nocache.py [port]
import sys
from http.server import SimpleHTTPRequestHandler, test


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, max-age=0")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    test(HandlerClass=NoCacheHandler, port=port)
