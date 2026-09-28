#!/usr/bin/env python3
"""Hollow skin editor server (stdlib only).

    python editor/serve.py [skin_dir] [--port 8765] [--open]

skin_dir defaults to FSVR's skin, plugin/skin in the FSVR repository beside this one.

Serves the editor files and one skin folder at a time on 127.0.0.1:
  GET/PUT/DELETE /skin/<path>, POST /skin/<path>?move=<newpath>
  GET/PUT/DELETE /work/<path>  (work = <skin_dir>/../tiles, for tile and glyph exports)
  GET /api/list[?root=work]    every file under the root with size and mtime
  GET /api/skins               the open skin, FSVR's, and the other folders opened since the start
  POST /api/open {"path"}      switch to another skin folder (one without skin.json starts a new skin)
  POST /api/browse             pick a folder in a native dialog, {"path": null} when cancelled
Writes and the POSTs need the header "X-Hollow: 1", so a web page from another origin cannot send them.
A page sends "X-Hollow-Skin: <token>" of the skin it loaded; after another tab switched skins its
requests get 409 instead of reaching the other skin.
"""
import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FSVR_SKIN = os.path.realpath(os.path.join(REPO, '..', 'FSVR', 'plugin', 'skin'))
BROWSE = """import tkinter, tkinter.filedialog
r = tkinter.Tk(); r.withdraw(); r.attributes('-topmost', True)
print(tkinter.filedialog.askdirectory(parent=r, title='Open a skin folder', mustexist=False) or '')"""
EDITOR_FILES = {'index.html', 'editor.js', 'editor.css', 'logic.mjs'}
TYPES = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
         '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.png': 'image/png', '.md': 'text/plain; charset=utf-8'}


def inside(root, rel):
    """Absolute path of the URL path `rel` under `root`, or None when it would escape `root`."""
    rel = unquote(rel)
    parts = [p for p in rel.split('/') if p not in ('', '.')]
    if any(p == '..' or '\\' in p or ':' in p or '\0' in p for p in parts):
        return None
    root = os.path.realpath(root)
    path = os.path.realpath(os.path.join(root, *parts))
    return path if path == root or path.startswith(root + os.sep) else None


def listing(root):
    out = []
    if root and os.path.isdir(root):
        for d, _, files in os.walk(root):
            for f in files:
                if f.endswith('.part'):
                    continue
                full = os.path.join(d, f)
                st = os.stat(full)
                out.append({'path': os.path.relpath(full, root).replace(os.sep, '/'), 'size': st.st_size, 'mtime': st.st_mtime})
    return sorted(out, key=lambda e: e['path'])


def prune(path, root):
    """Remove folders left empty by a delete or move, up to (not including) root."""
    d = os.path.dirname(path)
    while d != root and d.startswith(root + os.sep) and not os.listdir(d):
        os.rmdir(d)
        d = os.path.dirname(d)


def replace(src, dst):
    # Windows refuses to replace a file another process has open (the runtime reloading it); retry briefly.
    for attempt in range(20):
        try:
            return os.replace(src, dst)
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.05)


def skin_entry(path):
    """What the skin picker shows for a skin folder: its name, and whether it holds a skin yet."""
    name = None
    try:
        with open(os.path.join(path, 'skin.json'), encoding='utf-8') as f:
            name = json.load(f).get('name')
        exists = True
    except (OSError, ValueError):
        exists = False
    folder = os.path.basename(os.path.dirname(path)) if os.path.basename(path) == 'skin' else os.path.basename(path)
    rel = os.path.relpath(path, REPO).replace(os.sep, '/') if path.startswith(os.path.realpath(REPO) + os.sep) else path
    return {'path': path, 'rel': rel, 'name': name or folder, 'exists': exists}


class Handler(BaseHTTPRequestHandler):
    roots = {}
    token = ''
    recent = []  # folders other than FSVR's opened since the server started

    @classmethod
    def open(cls, skin):
        skin = os.path.realpath(skin)
        cls.roots = {'skin': skin, 'work': os.path.realpath(os.path.join(skin, '..', 'tiles'))}
        cls.token = secrets.token_hex(6)
        if skin != FSVR_SKIN and skin not in cls.recent:
            cls.recent.insert(0, skin)

    def skins(self):
        return {'current': skin_entry(self.roots['skin']), 'token': self.token,
                'skins': [skin_entry(p) for p in [FSVR_SKIN] + self.recent]}

    def log_request(self, code='-', size='-'):
        if str(getattr(code, 'value', code))[:1] not in '23':
            sys.stderr.write(f'{self.command} {self.path} -> {getattr(code, "value", code)}\n')

    def log_message(self, fmt, *args):
        pass

    def reply(self, code, body=b'', ctype='text/plain; charset=utf-8'):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def parse(self):
        u = urlsplit(self.path)
        head, _, rel = u.path.lstrip('/').partition('/')
        return head, rel, parse_qs(u.query)

    def local_host(self):
        return self.headers.get('Host', '').rsplit(':', 1)[0] in ('127.0.0.1', 'localhost')

    def stale(self):
        """True (after replying 409) when the page loaded a skin this server has since switched away from."""
        t = self.headers.get('X-Hollow-Skin')
        if t and t != self.token:
            self.reply(409, 'Another tab opened a different skin; reload this page.')
            return True
        return False

    def do_GET(self):
        if not self.local_host():
            return self.reply(403, 'bad host')
        head, rel, q = self.parse()
        if head == 'api' and rel == 'skins':
            return self.reply(200, json.dumps(self.skins()), TYPES['.json'])
        if self.stale():
            return
        if head == 'api' and rel == 'list':
            root = self.roots.get(q.get('root', ['skin'])[0])
            if not root:
                return self.reply(404, 'unknown root')
            return self.reply(200, json.dumps(listing(root)), TYPES['.json'])
        if head in self.roots:
            p = inside(self.roots[head], rel)
            if not p:
                return self.reply(403, 'path escapes its root')
            if not os.path.isfile(p):
                return self.reply(404, 'not found')
            with open(p, 'rb') as f:
                return self.reply(200, f.read(), TYPES.get(os.path.splitext(p)[1].lower(), 'application/octet-stream'))
        name = head or 'index.html'
        if name in EDITOR_FILES and not rel:
            with open(os.path.join(HERE, name), 'rb') as f:
                return self.reply(200, f.read(), TYPES[os.path.splitext(name)[1]])
        self.reply(404, 'not found')

    def target(self):
        """(root, absolute path, query) of a write request, or None after replying with the error."""
        if not self.local_host() or self.headers.get('X-Hollow') != '1':
            self.reply(403, 'forbidden')
            return None
        if self.stale():
            return None
        head, rel, q = self.parse()
        if head not in self.roots:
            self.reply(404, 'not found')
            return None
        root = os.path.realpath(self.roots[head])
        p = inside(root, rel)
        if not p or p == root:
            self.reply(403, 'path escapes its root')
            return None
        return root, p, q

    def do_PUT(self):
        t = self.target()
        if not t:
            return
        root, p, _ = t
        data = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(p), suffix='.part')
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        replace(tmp, p)
        self.reply(200, 'ok')

    def do_DELETE(self):
        t = self.target()
        if not t:
            return
        root, p, _ = t
        if not os.path.isfile(p):
            return self.reply(404, 'not found')
        os.remove(p)
        prune(p, root)
        self.reply(200, 'ok')

    def api(self, rel):
        if not self.local_host() or self.headers.get('X-Hollow') != '1':
            return self.reply(403, 'forbidden')
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        if rel == 'open':
            path = (json.loads(body or b'{}').get('path') or '').strip()
            path = os.path.realpath(os.path.expanduser(path)) if path else ''
            if not path or not (os.path.isdir(path) or os.path.isdir(os.path.dirname(path))):
                return self.reply(400, f'No such folder: {path}')
            self.open(path)
            return self.reply(200, json.dumps(self.skins()), TYPES['.json'])
        if rel == 'browse':
            try:
                r = subprocess.run([sys.executable, '-c', BROWSE], capture_output=True, text=True, timeout=600)
            except (OSError, subprocess.TimeoutExpired) as e:
                return self.reply(500, f'No folder dialog: {e}')
            if r.returncode:
                return self.reply(500, 'No folder dialog (this Python has no tkinter); type the path instead.')
            got = r.stdout.strip()
            return self.reply(200, json.dumps({'path': os.path.normpath(got) if got else None}), TYPES['.json'])
        self.reply(404, 'not found')

    def do_POST(self):
        if self.parse()[0] == 'api':
            return self.api(self.parse()[1])
        t = self.target()
        if not t:
            return
        root, p, q = t
        if 'move' not in q:
            return self.reply(400, 'POST needs ?move=<newpath>')
        dest = inside(root, q['move'][0])
        if not dest or dest == root:
            return self.reply(403, 'path escapes its root')
        if not os.path.isfile(p):
            return self.reply(404, 'not found')
        if os.path.exists(dest):
            return self.reply(409, 'target exists')
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        replace(p, dest)
        prune(p, root)
        self.reply(200, 'ok')


class Server(ThreadingHTTPServer):
    # On Windows SO_REUSEADDR lets a second server bind a port that is already listening, and the two
    # then share its connections; without it a busy port fails and main() picks a free one.
    allow_reuse_address = os.name != 'nt'


def main():
    ap = argparse.ArgumentParser(description='Serve the Hollow skin editor for one skin folder.')
    ap.add_argument('skin', nargs='?', default=FSVR_SKIN, help="skin folder (default: FSVR's, ../FSVR/plugin/skin)")
    ap.add_argument('--port', type=int, default=8765, help='port on 127.0.0.1 (0 picks a free one, and so does a busy one)')
    ap.add_argument('--open', action='store_true', help='open the editor in the default browser')
    a = ap.parse_args()
    Handler.open(a.skin)
    try:
        srv = Server(('127.0.0.1', a.port), Handler)
    except OSError:
        srv = Server(('127.0.0.1', 0), Handler)
    url = f'http://127.0.0.1:{srv.server_address[1]}/'
    print(f'Hollow skin editor: {url}  (skin {Handler.roots["skin"]})', flush=True)
    if a.open:
        print('Leave this window open while you edit; close it (or press Ctrl+C) to stop the editor.', flush=True)
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
