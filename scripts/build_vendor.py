#!/usr/bin/env python3
"""Build pinned third-party archives in the input project's dependency store."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / 'build/deps'
REPOS = {
    'utf8proc': ('https://github.com/JuliaStrings/utf8proc.git', 'b3e0f28adaec943ac25e3e27514dd6037e7a022e'),
    'brotli': ('https://github.com/google/brotli.git', '5fa73e23bee34f84148719576a7a434f0fc43dc8'),
    'thorvg': ('https://github.com/thorvg/thorvg.git', '54a98e42813a84d3ac2314b818725f9eea8b1658'),
}


def run(command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True, env=environment)


def checkout(name):
    url, revision = REPOS[name]
    source = DEPS / 'source' / name
    if not (source / '.git').is_dir():
        source.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '--no-checkout', url, str(source)])
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                                     text=True, stderr=subprocess.DEVNULL).strip() if \
        subprocess.run(['git', '-C', str(source), 'rev-parse', '--verify', 'HEAD'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0 else ''
    if actual != revision:
        run(['git', '-C', str(source), 'fetch', 'origin', revision])
        run(['git', '-C', str(source), 'checkout', '--detach', revision])
    return source


def build(name):
    source = checkout(name)
    build_dir = DEPS / 'vendor-build' / name
    prefix = DEPS / name
    prefix.mkdir(parents=True, exist_ok=True)
    if name == 'thorvg':
        if not (build_dir / 'build.ninja').exists():
            run(['meson', 'setup', str(build_dir), str(source), '--default-library=static',
                 '-Dengines=sw', '-Dloaders=svg,lottie,ttf,png,jpg', '-Dbindings=capi',
                 '-Dstatic=false', '-Dextra=lottie_expressions', '--libdir=lib', f'--prefix={prefix}'])
        run(['meson', 'compile', '-C', str(build_dir)])
        run(['meson', 'install', '-C', str(build_dir)])
    else:
        run(['cmake', '-S', str(source), '-B', str(build_dir), '-DCMAKE_BUILD_TYPE=Release',
             '-DBUILD_SHARED_LIBS=OFF', '-DCMAKE_INSTALL_LIBDIR=lib', f'-DCMAKE_INSTALL_PREFIX={prefix}'])
        run(['cmake', '--build', str(build_dir), '--parallel', str(os.cpu_count() or 1)])
        run(['cmake', '--install', str(build_dir)])


if __name__ == '__main__':
    environment = os.environ.copy()
    scratch = ROOT / 'temp/vendor-build'
    scratch.mkdir(parents=True, exist_ok=True)
    environment['TMPDIR'] = str(scratch)
    requested = sys.argv[1:] or list(REPOS)
    for item in requested:
        if item not in REPOS:
            raise SystemExit(f'unknown native dependency: {item}')
        build(item)
