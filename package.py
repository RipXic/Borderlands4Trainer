"""Put the built trainer into release\\package\\ and zip it as release\\BL4Trainer_<version>.zip (the same layout as the
release download: dsound.dll + INSTALL.txt + the BL4Trainer folder), then print SHA-256 checksums.
Usage: build.bat, then  python package.py 1.1.0"""
import hashlib
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.join(ROOT, 'release', 'package')
OUT = os.path.join(PKG, 'BL4Trainer')


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: python package.py <version>')
    ver = sys.argv[1]
    if os.path.isdir(PKG):
        shutil.rmtree(PKG)
    os.makedirs(OUT)
    for f, dst in (('native.dll', OUT), ('dsound.dll', PKG)):
        src = os.path.join(ROOT, 'build', f)
        if not os.path.isfile(src):
            sys.exit('missing ' + src + ' - run build.bat first')
        shutil.copy2(src, dst)
    data = os.path.join(ROOT, 'data')
    for e in os.listdir(data):
        s = os.path.join(data, e)
        shutil.copytree(s, os.path.join(OUT, e)) if os.path.isdir(s) else shutil.copy2(s, OUT)
    for name, dst in (('data_readme.txt', os.path.join(OUT, 'README.txt')), ('install.txt', os.path.join(PKG, 'INSTALL.txt'))):
        t = open(os.path.join(ROOT, name), encoding='utf-8').read().replace('{version}', ver)
        open(dst, 'w', encoding='utf-8', newline='\r\n').write(t)
    z = os.path.join(ROOT, 'release', 'BL4Trainer_' + ver + '.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fs in os.walk(PKG):
            for f in fs:
                p = os.path.join(dp, f)
                zf.write(p, os.path.relpath(p, PKG))
    print('zip', z)
    for p in (z, os.path.join(PKG, 'dsound.dll'), os.path.join(OUT, 'native.dll')):
        print('  sha256', os.path.basename(p), hashlib.sha256(open(p, 'rb').read()).hexdigest())


if __name__ == '__main__':
    main()
