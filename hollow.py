#!/usr/bin/env python3
"""Hollow launcher: edit FSVR's skin, then build and try it, from one place.

    python hollow.py                      a menu of everything below, in the order you need it
    python hollow.py editor [skin]        open the skin editor in your browser on FSVR's skin, or on
                                          [skin] (it can switch skins)
    python hollow.py build standalone     build the standalone app
    python hollow.py build all            build every format (on Windows 64-bit and 32-bit)
    python hollow.py run [--embedded]     start the FSVR standalone; it reads FSVR's skin folder and
                                          reloads on every save in the editor, unless --embedded
    python hollow.py bin                  open bin/, where every build is collected by format

The .cmd files next to this one (hollow.command on macOS) run these on a double-click.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
FSVR_SKIN = os.path.join(os.path.dirname(ROOT), 'FSVR', 'plugin', 'skin')   # the FSVR repository beside this one
BIN = os.path.join(ROOT, 'bin')
BUILD = os.path.join(ROOT, 'build')
WIN, MAC = os.name == 'nt', sys.platform == 'darwin'
BUNDLES = {'.exe': 'Standalone', '.app': 'Standalone', '.clap': 'CLAP', '.vst3': 'VST3', '.component': 'AU', '.vst': 'VST2'}
DLLS = {'x64': 'VST2 64-bit', 'x86': 'VST2 32-bit and DXi'}


def say(text=''):
    print(text, flush=True)


def products():
    """(folder, target, name) of every product under plugins/."""
    out = []
    for d in sorted(os.listdir(os.path.join(ROOT, 'plugins'))):
        try:
            with open(os.path.join(ROOT, 'plugins', d, 'CMakeLists.txt'), encoding='utf-8') as f:
                m = re.search(r'hollow_add_plugin\(\s*(\w+)\s+NAME\s+"([^"]+)"', f.read())
        except OSError:
            continue
        if m:
            out.append((d, m.group(1), m.group(2)))
    return out


def product(which):
    p = next((p for p in products() if which.lower() in (p[0].lower(), p[1].lower(), p[2].lower()) or which.lower() in p[0].lower()), None)
    return p or sys.exit(f'No product "{which}"; there are: ' + ', '.join(p[0] for p in products()))


def skin_ready():
    return os.path.isfile(os.path.join(FSVR_SKIN, 'skin.json'))


# ---- editor -------------------------------------------------------------------------------------------

def editor(skin=None, window=False):
    cmd = [sys.executable, os.path.join(ROOT, 'editor', 'serve.py'), *([os.path.abspath(skin)] if skin else []), '--open']
    if window and WIN:
        subprocess.Popen(cmd, creationflags=subprocess.CREATE_NEW_CONSOLE)
        return say('The editor opened in your browser. It runs in its own window; close that window to stop it.')
    say('Starting the editor. Press Ctrl+C here to stop it.')
    try:
        subprocess.call(cmd)
    except KeyboardInterrupt:
        pass


# ---- build --------------------------------------------------------------------------------------------

def find_cmake():
    exe = shutil.which('cmake')
    if exe or not WIN:
        return exe
    # Visual Studio ships its own CMake, which is often the only one on the machine.
    vswhere = os.path.expandvars(r'%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe')
    if os.path.isfile(vswhere):
        vs = subprocess.run([vswhere, '-latest', '-property', 'installationPath'], capture_output=True, text=True).stdout.strip()
        exe = os.path.join(vs, r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
        if vs and os.path.isfile(exe):
            return exe
    return None


def build(what):
    if what not in ('standalone', 'all'):
        sys.exit('build what? standalone or all')
    if not WIN and not MAC:
        sys.exit('Hollow builds on Windows and macOS.')
    cmake = find_cmake()
    if not cmake:
        sys.exit('CMake was not found. Install Visual Studio 2022 with "Desktop development with C++" (it includes CMake), '
                 'or CMake from https://cmake.org/download/' if WIN else
                 'CMake was not found: install it (https://cmake.org/download/ or "brew install cmake") and the Xcode command line tools.')
    if not skin_ready():
        say(f'Note: there is no skin at {FSVR_SKIN}, so FSVR builds with an empty window.')
    arches = ([('x64', ['-A', 'x64'])] + ([('x86', ['-A', 'Win32'])] if what == 'all' else [])) if WIN else [('mac', [])]
    targets = [f'{t}_standalone' for _, t, _ in products()] if what == 'standalone' else []
    start = time.time()
    for d, args in arches:
        bdir = os.path.join(BUILD, d)
        if not os.path.isfile(os.path.join(bdir, 'CMakeCache.txt')):
            say(f'\n== Configuring build/{d} (the first time downloads the plug-in SDKs, a few minutes)')
            if subprocess.call([cmake, '-S', ROOT, '-B', bdir, *args, '-DCMAKE_BUILD_TYPE=Release']):
                sys.exit(f'Configuring build/{d} failed (see above).')
        say(f'\n== Building {"the standalone" if targets else "every format"} in build/{d}')
        if subprocess.call([cmake, '--build', bdir, '--config', 'Release', '--parallel', *(['--target', *targets] if targets else [])]):
            sys.exit(f'The build in build/{d} failed (see above).')
    say(f'\nBuilt in {int(time.time() - start)} s.')
    collect()


def copy(src, dst):
    try:
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(src, dst)
        return True
    except OSError as e:
        say(f'  could not update {os.path.relpath(dst, ROOT)}: {e.strerror or e} (close it in the host or app using it, then build again)')
        return False


def collect():
    """Copy the plug-ins and apps out of build/*/out into bin/<format>/."""
    done = []
    for d in ('x64', 'x86', 'mac'):
        for top, dirs, files in os.walk(os.path.join(BUILD, d, 'out')):
            for n in sorted(dirs + files):
                ext = os.path.splitext(n)[1].lower()
                kind = BUNDLES.get(ext) or (DLLS.get(d) if ext == '.dll' else None)
                if kind:
                    os.makedirs(os.path.join(BIN, kind), exist_ok=True)
                    if copy(os.path.join(top, n), os.path.join(BIN, kind, n)):
                        done.append(f'{kind}/{n}')
            dirs[:] = [x for x in dirs if os.path.splitext(x)[1].lower() not in BUNDLES]  # never inside a bundle
    if done:
        say('Collected in bin/:\n' + '\n'.join(f'  {p}' for p in sorted(set(done))))
        say('Copy a format\'s files to where your host looks for plug-ins (for VST3, C:\\Program Files\\Common Files\\VST3).' if WIN else
            'Copy a format\'s files to ~/Library/Audio/Plug-Ins/<format>/ for your host.')


def find_app(which):
    _, _, name = product(which)
    for p in [os.path.join(BIN, 'Standalone', name + ('.exe' if WIN else '.app'))] + glob.glob(os.path.join(BUILD, '*', 'out', '**', name + ('.exe' if WIN else '.app')), recursive=True):
        if os.path.exists(p):
            return p
    return None


def run_app(embedded=False):
    _, _, name = product('fsvr')
    app = find_app('fsvr')
    if not app:
        sys.exit(f'{name} is not built yet: build the standalone first (python hollow.py build standalone).')
    env = dict(os.environ)
    live = not embedded and skin_ready()
    if live:
        env['HOLLOW_SKIN_DIR'] = FSVR_SKIN
    exe = app if WIN else (glob.glob(os.path.join(app, 'Contents', 'MacOS', '*')) or [app])[0]
    subprocess.Popen([exe], env=env, cwd=os.path.dirname(exe))
    say(f'Started {name}.' + (f' It shows {FSVR_SKIN} live: save in the editor and it updates within a second.' if live else ''))


def open_bin():
    os.makedirs(BIN, exist_ok=True)
    if WIN:
        os.startfile(BIN)
    else:
        subprocess.call(['open' if MAC else 'xdg-open', BIN])


# ---- menu ---------------------------------------------------------------------------------------------

def when(path):
    return time.strftime('built %d %b %H:%M', time.localtime(os.path.getmtime(path))) if path else 'not built'


def menu():
    everything = 'CLAP, VST3, VST2 64-bit, VST2 32-bit + DXi, standalone' if WIN else 'CLAP, VST3, AU, VST2, standalone (universal)'
    items = [
        ('1', 'Open the skin editor', 'edit FSVR\'s skin in your browser; the Skin button switches skins', lambda: editor(window=True)),
        ('2', 'Build the standalone', 'the FSVR app (first build: minutes)', lambda: build('standalone')),
        ('3', 'Run FSVR', 'the standalone; skin edits show up as you save', run_app),
        ('4', 'Build everything', everything, lambda: build('all')),
        ('5', 'Open the plug-ins folder', 'bin/, every build sorted by format', open_bin),
    ]
    while True:
        os.system('cls' if WIN else 'clear')
        app = find_app('fsvr')
        full = os.path.isdir(os.path.join(BIN, 'VST3'))
        nxt = '2' if not app else '3'
        say(f'Hollow   {ROOT}\n')
        say(f'  FSVR skin        {FSVR_SKIN}' + ('' if skin_ready() else ' (missing)'))
        say(f'  Standalone       {when(app)}')
        say(f'  All formats      {when(os.path.join(BIN, "VST3") if full else None)}\n')
        for key, title, about, _ in items:
            say(f'  {key:>2}  {title:<26} {about}{"   <- next" if key == nxt else ""}')
        say('   q  Quit\n')
        try:
            choice = input('Choose: ').strip().lower()
        except (EOFError, KeyboardInterrupt):
            return
        if choice in ('q', 'quit', 'exit'):
            return
        fn = next((f for k, _, _, f in items if k == choice), None)
        if not fn:
            continue
        say()
        try:
            fn()
        except SystemExit as e:
            if e.code not in (None, 0):
                say(str(e.code))
        except KeyboardInterrupt:
            say('\nStopped.')
        try:
            input('\nPress Enter to return to the menu...')
        except (EOFError, KeyboardInterrupt):
            return


def main(argv):
    if not argv:
        return menu()
    cmd, rest = argv[0], argv[1:]
    if cmd == 'editor':
        editor(rest[0] if rest else None)
    elif cmd == 'build':
        build(rest[0] if rest else 'all')
    elif cmd == 'run':
        run_app('--embedded' in rest)
    elif cmd == 'bin':
        open_bin()
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
