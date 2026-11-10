#!/usr/bin/env python3
"""Build the pinned input-owned dependencies using their JSON recipes."""
import os
from pathlib import Path
import re
import subprocess
import sys

from build_config import load_build_config
from generate_image_config import write_changed

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / 'build/deps'
CONFIG = ROOT / 'build_vendor_deps_config.json'


def run(command, environment):
    subprocess.run(command, cwd=ROOT, check=True, env=environment)


def checkout(name, spec, environment):
    source = DEPS / 'source' / name
    created = not (source / '.git').is_dir()
    if created:
        source.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '--no-checkout', spec['url'], str(source)], environment)
    else:
        changes = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain',
                                           '--untracked-files=no'], text=True)
        if changes:
            raise ValueError(f'{source}: pinned dependency source has local changes')
    actual = subprocess.run(['git', '-C', str(source), 'rev-parse', '--verify', 'HEAD'],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if actual.returncode or actual.stdout.strip() != spec['revision']:
        run(['git', '-C', str(source), 'fetch', 'origin', spec['revision']], environment)
    if created or actual.returncode or actual.stdout.strip() != spec['revision']:
        run(['git', '-C', str(source), 'checkout', '--detach', spec['revision']], environment)
    if created:
        changes = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain',
                                           '--untracked-files=no'], text=True)
        if changes:
            raise ValueError(f'{source}: pinned dependency source has local changes')
    return source


def source_version(source, spec):
    content = (source / spec['version_file']).read_text()
    if 'version_pattern' in spec:
        match = re.search(spec['version_pattern'], content)
        if not match:
            raise ValueError(f'{source}: missing upstream version')
        return match.group(1)
    parts = []
    for component in ('MAJOR', 'MINOR', 'PATCH'):
        name = spec['version_prefix'] + '_' + component
        match = re.search(r'^\s*#\s*define\s+' + re.escape(name) + r'\s+(\d+)\b', content, re.M)
        if not match:
            raise ValueError(f'{source}: missing upstream {name}')
        parts.append(match.group(1))
    return '.'.join(parts)


def install_metadata(name, spec, source, prefix):
    version = source_version(source, spec)
    extra = []
    if name == 'thorvg':
        # The archive, rather than the host compiler name, determines its OpenMP ABI.
        symbols = subprocess.check_output(['nm', '-u', str(prefix / 'lib/libthorvg.a')], text=True)
        if 'GOMP_' in symbols:
            extra.append('-lgomp')
        if '__kmpc_' in symbols:
            extra.append('-lomp')
    for package in spec['packages']:
        lines = [f'prefix={prefix}', 'libdir=${prefix}/lib', 'includedir=${prefix}/include',
                 '', f'Name: {package["name"]}', f'Description: {package["description"]}',
                 f'Version: {version}']
        for key, field in (('requires', 'Requires'), ('requires_private', 'Requires.private')):
            if package.get(key):
                lines.append(field + ': ' + ', '.join(package[key]))
        libs = ['-L${libdir}', '-l' + package['library'], *package.get('libs', []), *extra]
        lines.append('Libs: ' + ' '.join(libs))
        if package.get('libs_private'):
            lines.append('Libs.private: ' + ' '.join(package['libs_private']))
        lines.append('Cflags: ' + ' '.join(['-I${includedir}', *package.get('cflags', [])]))
        write_changed(prefix / 'lib/pkgconfig' / (package['name'] + '.pc'), '\n'.join(lines) + '\n')


def main():
    config = load_build_config(CONFIG)
    vendors = config['vendor_sources']
    requested = sys.argv[1:] or list(vendors)
    environment = dict(os.environ, TMPDIR=str(ROOT / 'temp/vendor-build'))
    Path(environment['TMPDIR']).mkdir(parents=True, exist_ok=True)
    jobs = int(os.environ.get('JOBS', os.cpu_count() or 1))
    if jobs < 1:
        raise ValueError('vendor build: JOBS must be positive')
    for name in requested:
        if name not in vendors:
            raise ValueError(f'unknown native dependency: {name}')
        spec = vendors[name]
        source = checkout(name, spec, environment)
        prefix = DEPS / name
        run([sys.executable, str(ROOT / 'scripts/dependency_builder.py'), name,
             '--source', str(source), '--prefix', str(prefix), '--jobs', str(jobs)], environment)
        install_metadata(name, spec, source, prefix)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'input vendor build: {error}', file=sys.stderr)
        sys.exit(1)
