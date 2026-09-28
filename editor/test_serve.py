"""python editor/test_serve.py : smoke test of serve.py against a small throwaway skin."""
import http.client
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FSVR_SKIN = os.path.realpath(os.path.join(HERE, '..', '..', 'FSVR', 'plugin', 'skin'))   # listed first, whatever is open


def main():
    with tempfile.TemporaryDirectory() as tmp:
        skin = os.path.join(tmp, 'skin')
        os.makedirs(os.path.join(skin, 'views'))
        with open(os.path.join(skin, 'skin.json'), 'w') as f:
            json.dump({'format': 1, 'name': 'T', 'root': 'main', 'images': {}}, f)
        with open(os.path.join(skin, 'views', 'main.json'), 'w') as f:
            json.dump({'size': [10, 10], 'widgets': []}, f)
        with open(os.path.join(tmp, 'secret.txt'), 'w') as f:
            f.write('outside')
        proc = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve.py'), skin, '--port', '0'], stdout=subprocess.PIPE, text=True)
        try:
            port = int(re.search(r'127\.0\.0\.1:(\d+)', proc.stdout.readline()).group(1))
            run(port, skin, tmp)
        finally:
            proc.terminate()
            proc.wait(5)
    print('serve.py smoke test passed')


def run(port, skin, tmp):
    base = f'http://127.0.0.1:{port}'

    def call(method, path, body=None, write=True, headers=None):
        req = urllib.request.Request(base + path, data=body, method=method, headers={**({'X-Hollow': '1'} if write else {}), **(headers or {})})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, r.read(), r.headers.get('Content-Type', '')
        except urllib.error.HTTPError as e:
            return e.code, e.read(), ''

    def raw(method, path, headers=None):
        """Send the path exactly as written (no client-side normalizing)."""
        c = http.client.HTTPConnection('127.0.0.1', port)
        c.request(method, path, headers={'X-Hollow': '1', **(headers or {})})
        r = c.getresponse()
        r.read()
        c.close()
        return r.status

    # editor files, with module-friendly content types
    code, body, ctype = call('GET', '/', write=False)
    assert code == 200 and b'editor.js' in body and ctype.startswith('text/html'), (code, ctype)
    for name in ('editor.js', 'logic.mjs'):
        code, _, ctype = call('GET', '/' + name, write=False)
        assert code == 200 and ctype.startswith('text/javascript'), (name, code, ctype)
    assert call('GET', '/serve.py', write=False)[0] == 404

    # listing and reading
    code, body, _ = call('GET', '/api/list', write=False)
    listed = {e['path']: e for e in json.loads(body)}
    assert set(listed) == {'skin.json', 'views/main.json'} and listed['skin.json']['size'] > 0 and listed['skin.json']['mtime'] > 0, listed
    assert json.loads(call('GET', '/skin/skin.json', write=False)[1])['root'] == 'main'
    assert call('GET', '/skin/nothing.json', write=False)[0] == 404

    # writing creates folders; writes need the X-Hollow header
    assert call('PUT', '/skin/images/knobs/a.png', b'\x89PNG data')[0] == 200
    assert open(os.path.join(skin, 'images', 'knobs', 'a.png'), 'rb').read() == b'\x89PNG data'
    assert call('GET', '/skin/images/knobs/a.png', write=False)[1] == b'\x89PNG data'
    assert call('PUT', '/skin/views/x.json', b'{}', write=False)[0] == 403
    assert call('DELETE', '/skin/views/main.json', write=False)[0] == 403

    # move (rename), refusing an existing target
    assert call('POST', '/skin/images/knobs/a.png?move=images/dials/b.png')[0] == 200
    assert call('GET', '/skin/images/knobs/a.png', write=False)[0] == 404
    assert call('GET', '/skin/images/dials/b.png', write=False)[1] == b'\x89PNG data'
    assert not os.path.exists(os.path.join(skin, 'images', 'knobs')), 'empty folder left behind'
    assert call('POST', '/skin/images/dials/b.png?move=views/main.json')[0] == 409
    assert call('POST', '/skin/images/dials/b.png')[0] == 400

    # delete
    assert call('DELETE', '/skin/images/dials/b.png')[0] == 200
    assert call('DELETE', '/skin/images/dials/b.png')[0] == 404
    assert not os.path.exists(os.path.join(skin, 'images')), 'empty folders left behind'

    # the work folder sits next to the skin
    assert call('PUT', '/work/knobs/a/000.png', b'tile')[0] == 200
    assert open(os.path.join(tmp, 'tiles', 'knobs', 'a', '000.png'), 'rb').read() == b'tile'
    assert [e['path'] for e in json.loads(call('GET', '/api/list?root=work', write=False)[1])] == ['knobs/a/000.png']
    assert call('GET', '/work/knobs/a/000.png', write=False)[1] == b'tile'
    assert call('GET', '/api/list?root=elsewhere', write=False)[0] == 404

    # nothing escapes its root
    for method, path in [('GET', '/skin/../secret.txt'), ('GET', '/skin/..%2fsecret.txt'), ('GET', '/skin/%2e%2e/secret.txt'),
                         ('GET', '/skin/views/..%5c..%5csecret.txt'), ('GET', '/work/../skin/skin.json'), ('PUT', '/skin/../x.json'),
                         ('PUT', '/skin/C:/x.json'), ('DELETE', '/skin/..%2fsecret.txt'), ('POST', '/skin/skin.json?move=../moved.json'),
                         ('POST', '/skin/skin.json?move=..%2fmoved.json'), ('DELETE', '/skin/')]:
        code = raw(method, path)
        assert code == 403, (method, path, code)
    assert os.path.exists(os.path.join(tmp, 'secret.txt')) and os.path.exists(os.path.join(skin, 'skin.json'))
    assert raw('GET', '/skin/skin.json', {'Host': 'evil.example:80'}) == 403

    # switching skins: needs the write header, refuses a missing parent, and turns away the old tab
    info = json.loads(call('GET', '/api/skins', write=False)[1])
    assert info['current']['name'] == 'T' and info['current']['exists'] and info['skins'][0]['path'] == FSVR_SKIN, info
    old = {'X-Hollow-Skin': info['token']}
    other = os.path.join(tmp, 'other')
    assert call('POST', '/api/open', json.dumps({'path': other}).encode(), write=False)[0] == 403
    assert call('POST', '/api/open', json.dumps({'path': os.path.join(tmp, 'no', 'such')}).encode())[0] == 400
    code, body, _ = call('POST', '/api/open', json.dumps({'path': other}).encode())
    info = json.loads(body)
    assert code == 200 and info['current']['path'] == os.path.realpath(other) and not info['current']['exists'], info
    assert call('GET', '/api/list', write=False, headers=old)[0] == 409
    assert call('PUT', '/skin/skin.json', b'{}', headers=old)[0] == 409
    assert call('PUT', '/skin/skin.json', b'{}', headers={'X-Hollow-Skin': info['token']})[0] == 200
    assert os.path.isfile(os.path.join(other, 'skin.json')) and json.loads(call('GET', '/skin/skin.json', write=False)[1]) == {}


if __name__ == '__main__':
    main()
