"""Put the built trainer and its data into release\\BL4Trainer\\ and zip it as release\\BL4Trainer_<version>.zip
(the same layout as the release download), then print SHA-256 checksums.
Usage: build.bat, then  python package.py 1.0.0"""
import hashlib
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, 'release', 'BL4Trainer')


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: python package.py <version>')
    ver = sys.argv[1]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    for f in ('native.dll', 'BL4Trainer.exe'):
        src = os.path.join(ROOT, 'build', f)
        if not os.path.isfile(src):
            sys.exit('missing ' + src + ' - run build.bat first')
        shutil.copy2(src, OUT)
    data = os.path.join(ROOT, 'data')
    for e in os.listdir(data):
        s = os.path.join(data, e)
        shutil.copytree(s, os.path.join(OUT, e)) if os.path.isdir(s) else shutil.copy2(s, OUT)
    readme = open(os.path.join(ROOT, 'data_readme.txt'), encoding='utf-8').read().replace('{version}', ver)
    open(os.path.join(OUT, 'README.txt'), 'w', encoding='utf-8', newline='\r\n').write(readme)
    z = os.path.join(ROOT, 'release', 'BL4Trainer_' + ver + '.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fs in os.walk(OUT):
            for f in fs:
                p = os.path.join(dp, f)
                zf.write(p, os.path.join('BL4Trainer', os.path.relpath(p, OUT)))
    print('zip', z)
    for p in (z, os.path.join(OUT, 'native.dll'), os.path.join(OUT, 'BL4Trainer.exe')):
        print('  sha256', os.path.basename(p), hashlib.sha256(open(p, 'rb').read()).hexdigest())


if __name__ == '__main__':
    main()
