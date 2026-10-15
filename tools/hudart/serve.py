"""Serve the repository for the HUD mockup (tools/hudart/mockup/), every response marked `Cache-Control: no-cache`.
usage: serve.py [--bind ADDR] [--port N]        (defaults 127.0.0.1 and 8000)

The page is made of files that change together: layout.json, kit.json, the parts they name, hud.js and hud.css.
`python3 -m http.server` sends no caching headers, so a browser keeps each file for a heuristic time of its own
and can draw new parts against an old kit.json or an old hud.js: slices in the wrong place, bands stretched that
should repeat. With no-cache the browser revalidates every file on every load (unchanged files cost a 304), so
a reload always draws one consistent version."""
import argparse
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NoCache(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=8000)
    a = ap.parse_args()
    ThreadingHTTPServer((a.bind, a.port), functools.partial(NoCache, directory=str(ROOT))).serve_forever()
