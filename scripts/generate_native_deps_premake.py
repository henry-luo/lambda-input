#!/usr/bin/env python3
"""Generate Premake projects for input's profile-specific native archives."""
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

from build_config import load_build_config, resolve_source_sets
from build_sources import expand_sources
from dependency_builder import generate_premake, projects_for, reset_build_inputs

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / 'build_native_deps_config.json'


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: generate_native_deps_premake.py PROFILE')
    profile = sys.argv[1]
    if profile not in ('debug', 'release', 'release_profile', 'debug_asan', 'coverage'):
        raise ValueError(f'native dependencies: unsupported profile {profile}')
    system = {'Darwin': 'macos', 'Linux': 'linux', 'Windows': 'windows'}.get(platform.system())
    if not system and os.environ.get('MSYSTEM'):
        system = 'windows'
    if not system:
        raise ValueError(f'native dependencies: unsupported system {platform.system()}')
    machine = platform.machine().lower()
    architecture = {'aarch64': 'arm64', 'amd64': 'x86_64', 'x64': 'x86_64'}.get(machine, machine)
    config = load_build_config(CONFIG)
    build = ROOT / 'temp/native-deps' / profile
    build.mkdir(parents=True, exist_ok=True)
    obj_root = ROOT / 'build/obj/native-deps-premake' / profile
    target_dir = ROOT / 'build/lib/native' / profile
    native_flags = shlex.split(os.environ.get('NATIVE_FLAGS', ''))
    targets = []
    for original in config['targets']:
        target = copy.deepcopy(original)
        resolve_source_sets(target, system, architecture)
        name = target['name']
        sources = expand_sources(ROOT, target)
        if not sources:
            raise ValueError(f'{name}: empty native source set')
        target['source_files'] = [str(ROOT / path) for path in sources]
        target.pop('source_patterns', None)
        target['includes'] = [str(ROOT), *[str(ROOT / path) for path in target.get('includes', [])],
                              '/opt/homebrew/include', '/usr/local/include']
        for rule in target.get('file_rules', []):
            rule['files'] = [str(ROOT / path) for path in rule['files']]
        target['target_dir'] = str(target_dir)
        target['obj_dir'] = str(obj_root / name)
        target['build_options'] = list(native_flags)
        if target.pop('optimize', None) == 'Size':
            target['build_options'].append('-Os')
        # Static archives expose their link requirements to the final consumer.
        target.pop('libraries', None)
        targets.append(target)
    variables = {'root': str(ROOT), 'input_root': str(ROOT), 'build': str(build),
                 'system': system, 'architecture': architecture, 'configuration': profile,
                 'workspace_optimize': None, 'workspace_defines': [],
                 'platform_flags': shlex.split(os.environ.get('PLATFORM_FLAGS', ''))}
    projects = projects_for({'projects': targets}, variables)
    compiler = os.environ.get('CC') or 'cc'
    cxx = os.environ.get('CXX') or 'c++'
    premake = os.environ.get('PREMAKE5_BIN') or 'premake5'
    identity = json.dumps({'projects': projects, 'config': hashlib.sha256(CONFIG.read_bytes()).hexdigest(),
                           'generator': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                           'builder': hashlib.sha256((ROOT / 'scripts/dependency_builder.py').read_bytes()).hexdigest(),
                           'compiler': subprocess.check_output([*shlex.split(compiler), '--version'], text=True),
                           'cxx': subprocess.check_output([*shlex.split(cxx), '--version'], text=True),
                           'premake': subprocess.check_output([premake, '--version'], text=True),
                           'environment': {key: os.environ.get(key, '') for key in
                                           ('CC', 'CXX', 'AR', 'RANLIB', 'CPPFLAGS', 'NATIVE_FLAGS', 'PLATFORM_FLAGS')}},
                          sort_keys=True)
    archives = [target_dir / ('lib' + project['name'] + '.a') for project in projects]
    # Make cannot infer compiler/flag changes from object timestamps.
    reset_build_inputs(build / 'build-inputs.json', identity, [obj_root, *archives])
    generate_premake({}, variables, projects, compiler, build / 'premake5.lua')
    print(f'NATIVE_DEPS: {system}/{architecture} {profile}, {len(projects)} owned archives')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'native dependencies: {error}', file=sys.stderr)
        sys.exit(1)
