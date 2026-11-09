#!/usr/bin/env python3
"""Generate native dependency archive rules from their owning manifest."""
import copy
import fnmatch
import os
from pathlib import Path
import platform
import shlex
import sys

from build_config import load_build_config, resolve_source_sets
from build_sources import expand_sources
from generate_image_config import write_changed

ROOT = Path(__file__).resolve().parent.parent


def quote(value):
    return shlex.quote(value.replace('\\"', '"'))


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: generate_native_deps_makefile.py PROFILE')
    profile = sys.argv[1]
    if profile not in ('debug', 'release', 'release_profile', 'debug_asan', 'coverage'):
        raise SystemExit(f'native dependencies: unsupported profile {profile}')
    system = {'Darwin': 'macos', 'Linux': 'linux', 'Windows': 'windows'}.get(platform.system())
    if not system:
        raise SystemExit(f'native dependencies: unsupported system {platform.system()}')
    architecture = {'aarch64': 'arm64', 'amd64': 'x86_64', 'x64': 'x86_64'}.get(
        platform.machine().lower(), platform.machine().lower())
    config = load_build_config(ROOT / 'build_native_deps_config.json')
    lines = [
        '# Generated from build_native_deps_config.json; do not edit.',
        'CC ?= cc', 'CXX ?= c++', 'AR ?= ar',
        'NATIVE_FLAGS ?= -O3 -g -DDEBUG -DLAMBDA_JS_EXEC_PROFILE -fPIC -fno-omit-frame-pointer',
        'PLATFORM_FLAGS ?=',
        '.PHONY: all',
    ]
    archives = []
    depfiles = []
    for original in config['targets']:
        target = copy.deepcopy(original)
        resolve_source_sets(target, system, architecture)
        name = target['name']
        sources = expand_sources(ROOT, target)
        if not sources:
            raise ValueError(f'{name}: empty native source set')
        objects = [f'build/obj/native-deps/{profile}/{name}/{path.replace("/", "__")}.o' for path in sources]
        if len(objects) != len(set(objects)):
            raise ValueError(f'{name}: native object-name collision')
        archive = f'build/lib/native/{profile}/lib{name}.a'
        archives.append(archive)
        # Owned headers must precede installed versions of the same library.
        includes = ['.', *target.get('includes', []), '/opt/homebrew/include', '/usr/local/include']
        base = [*(f'-I{include}' for include in includes),
                *(f'-D{define}' for define in target.get('defines', []))]
        if target.get('optimize') == 'Size':
            base.append('-Os')
        for source, obj in zip(sources, objects):
            flags = list(base)
            for rule in target.get('file_rules', []):
                if any(fnmatch.fnmatchcase(source, pattern) for pattern in rule.get('files', [])):
                    flags.extend(f'-D{define}' for define in rule.get('defines', []))
                    flags.extend(rule.get('build_options', []))
            cpp = Path(source).suffix in ('.cc', '.cpp', '.cxx')
            compiler = '$(CXX)' if cpp else '$(CC)'
            standard = '-std=c++17' if cpp else '-std=c11'
            depfiles.append(obj + '.d')
            lines.extend([
                f'{obj}: {source} build_native_deps_config.json build/native-deps.mk build/native-{profile}-build.json',
                '\t@mkdir -p $(@D)',
                f'\t{compiler} $(CPPFLAGS) $(PLATFORM_FLAGS) $(NATIVE_FLAGS) '
                + ' '.join(quote(flag) for flag in flags)
                + f' {standard} -MMD -MP -MF $@.d -c $< -o $@',
            ])
        lines.extend([
            f'{archive}: ' + ' '.join(objects) + ' build/native-deps.mk',
            '\t@mkdir -p $(@D)',
            '\trm -f $@',
            '\t$(AR) rcs $@ ' + ' '.join(objects),
        ])
    lines.extend(['all: ' + ' '.join(archives), '-include ' + ' '.join(depfiles), ''])
    write_changed(ROOT / 'build/native-deps.mk', '\n'.join(lines))
    print(f'NATIVE_DEPS: {system}/{architecture} {profile}, {len(archives)} owned archives')


if __name__ == '__main__':
    os.chdir(ROOT)
    main()
