#!/usr/bin/env python3
"""PROTOTYPE — throwaway. Static server for the cuts page, with Range support.

THREADING matters as much as ranges here: the compare page pulls five videos at
once, and a single-threaded HTTPServer serialises them — every tile stays black
while the first file is still streaming, which looks exactly like broken video.

`http.server` does not answer Range requests, and a <video> that cannot be range-
served cannot be SEEKED in Chrome — which is the one thing this page is for. So the
handler below implements 206 Partial Content and nothing else.

    promo/prototype-cuts/serve.py <dir> [port]
"""

import os
import re
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

RANGE = re.compile(r"bytes=(\d*)-(\d*)")


class RangeHandler(SimpleHTTPRequestHandler):
    def send_head(self):
        hdr = self.headers.get("Range")
        if not hdr:
            return super().send_head()
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            return super().send_head()
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404)
            return None
        size = os.fstat(f.fileno()).st_size
        m = RANGE.match(hdr.strip())
        if not m:
            f.close()
            self.send_error(400)
            return None
        start, end = m.group(1), m.group(2)
        if start == "":                       # suffix range: last N bytes
            start = max(0, size - int(end)); end = size - 1
        else:
            start = int(start)
            end = int(end) if end else size - 1
        end = min(end, size - 1)
        if start > end:
            f.close()
            self.send_error(416)
            return None
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        f.seek(start)
        return _Slice(f, end - start + 1)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *a):
        pass


class _Slice:
    """A file object that stops after n bytes, so copyfile sends just the range."""

    def __init__(self, f, n):
        self.f, self.n = f, n

    def read(self, amt=-1):
        if self.n <= 0:
            return b""
        if amt is None or amt < 0 or amt > self.n:
            amt = self.n
        b = self.f.read(amt)
        self.n -= len(b)
        return b

    def close(self):
        self.f.close()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    handler = partial(RangeHandler, directory=root)
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler)
    print(f"serving {root} at http://127.0.0.1:{port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
