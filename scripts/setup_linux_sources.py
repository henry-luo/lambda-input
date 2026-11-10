#!/usr/bin/env python3
"""Prepare Linux-only native providers with the input-owned Premake recipes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

from dependency_builder import CompilerProbe
from generate_image_config import write_changed

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / 'build/deps'
CONFIG = ROOT / 'build_vendor_deps_config.json'


def source(name, spec):
    basename = f'{name}-{spec["version"]}'
    archive = DEPS / 'downloads' / f'{basename}.{spec["extension"]}'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        with tempfile.NamedTemporaryFile(dir=ROOT / 'temp', delete=False) as output:
            temporary = Path(output.name)
            try:
                with urllib.request.urlopen(spec['url'], timeout=60) as response:
                    shutil.copyfileobj(response, output)
            except BaseException:
                temporary.unlink(missing_ok=True)
                raise
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
        raise ValueError(f'checksum mismatch: {archive}')
    source_dir = DEPS / 'source' / basename
    stamp = DEPS / 'source' / (basename + '.sha256')
    if source_dir.exists():
        if not stamp.exists() or stamp.read_text().strip() != spec['sha256']:
            raise ValueError(f'inspect unverified source tree: {source_dir}')
    else:
        source_dir.parent.mkdir(parents=True, exist_ok=True)
        # Validate archive member paths and links before extraction.
        with tarfile.open(archive) as tar:
            for member in tar:
                parts = Path(member.name).parts
                if not parts or parts[0] != basename or '..' in parts or member.issym() or member.islnk():
                    raise ValueError(f'unsafe archive member: {member.name}')
            with tempfile.TemporaryDirectory(dir=ROOT / 'temp', prefix='setup-source-') as directory:
                tar.extractall(directory)
                (Path(directory) / basename).rename(source_dir)
        stamp.write_text(spec['sha256'] + '\n')
    with tarfile.open(archive) as tar:
        for member in tar:
            if not member.isfile():
                continue
            path = source_dir.joinpath(*Path(member.name).parts[1:])
            if not path.is_file() or path.stat().st_size != member.size:
                raise ValueError(f'source differs from pinned archive: {path}')
            with tar.extractfile(member) as expected, path.open('rb') as actual:
                if hashlib.sha256(expected.read()).digest() != hashlib.sha256(actual.read()).digest():
                    raise ValueError(f'source differs from pinned archive: {path}')
    return source_dir


def main():
    (ROOT / 'temp').mkdir(parents=True, exist_ok=True)
    probe_dir = ROOT / 'temp/linux-support-probe'
    probe_dir.mkdir(parents=True, exist_ok=True)
    probe = CompilerProbe(os.environ.get('CC', 'cc'), probe_dir)
    if not probe.accepts('#include <stddef.h>\n_Static_assert(sizeof(size_t) == 8, "64-bit required");\n__uint128_t value;\n'):
        raise ValueError('Linux support recipes require a 64-bit compiler with __uint128_t')
    config = json.loads(CONFIG.read_text())['linux_setup_sources']
    prefix = DEPS / 'linux-support'
    for name, spec in config.items():
        path = source(name, spec)
        subprocess.run([sys.executable, str(ROOT / 'scripts/dependency_builder.py'), name,
                        '--source', str(path), '--prefix', str(prefix),
                        '--jobs', os.environ.get('JOBS', str(os.cpu_count() or 1))], cwd=ROOT, check=True)
        package = 'libmpdec' if name == 'mpdecimal' else 'mbedtls'
        libraries = '-lmpdec -lm' if name == 'mpdecimal' else '-lmbedtls -lmbedx509 -lmbedcrypto'
        metadata = (f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n'
                    f'Name: {package}\nDescription: input-owned {name}\nVersion: {spec["version"]}\n'
                    f'Libs: -L${{libdir}} {libraries}\nCflags: -I${{includedir}}\n')
        write_changed(prefix / 'lib/pkgconfig' / (package + '.pc'), metadata)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'input Linux setup: {error}', file=sys.stderr)
        sys.exit(1)
