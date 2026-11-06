"""pack_release.py: release/MouseWindowFixes-<version>.zip (MouseWindowFixes.asi at the root, as DMM and ASI
loaders take it, with the licences in a licenses folder) and release/MouseWindowFixes-<version>-source.zip (the
project's tracked files). The version comes from src/mouse_window_fixes.rc. Build first (build.cmd)."""
import os, re, subprocess, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'release')
ASI = os.path.join(ROOT, 'build', 'MouseWindowFixes.asi')

ver = re.search(r'#define MWF_VERSION_STR "([^"]+)"', open(os.path.join(ROOT, 'src', 'mouse_window_fixes.rc')).read()).group(1)
assert os.path.exists(ASI), 'build first'
os.makedirs(OUT, exist_ok=True)

main = os.path.join(OUT, f'MouseWindowFixes-{ver}.zip')
with zipfile.ZipFile(main, 'w', zipfile.ZIP_DEFLATED) as z:
    z.write(ASI, 'MouseWindowFixes.asi')
    z.write(os.path.join(ROOT, 'LICENSE.txt'), 'licenses/LICENSE.txt')
    z.write(os.path.join(ROOT, 'THIRD-PARTY-MinHook-LICENSE.txt'), 'licenses/THIRD-PARTY-MinHook-LICENSE.txt')
print(main, os.path.getsize(main) // 1024, 'KB')

files = subprocess.run(['git', '-C', ROOT, 'ls-files'], capture_output=True, text=True, check=True).stdout.split()
src = os.path.join(OUT, f'MouseWindowFixes-{ver}-source.zip')
with zipfile.ZipFile(src, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in files:
        z.write(os.path.join(ROOT, f), f'MouseWindowFixes-{ver}-source/{f}')
print(src, os.path.getsize(src) // 1024, 'KB', len(files), 'files')
