#!/usr/bin/env python3
"""Build source dependencies from owner JSON recipes with Premake and Make."""

import argparse
import copy
import glob
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

from build_config import load_build_config, resolve_source_sets
from generate_image_config import write_changed

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / 'build_vendor_deps_config.json'


def expand(value, variables):
    if isinstance(value, dict):
        return {key: expand(item, variables) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, variables) for item in value]
    if isinstance(value, str):
        for key, item in variables.items():
            value = value.replace('${' + key + '}', str(item))
    return value


def run(command, log, cwd=ROOT, env=None):
    with Path(log).open('w') as output:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'command failed; see {log}')


class CompilerProbe:
    def __init__(self, compiler, directory, environment=None):
        self.command = shlex.split(compiler)
        self.directory = directory
        self.cache = {}
        self.environment = dict(environment or os.environ, TMPDIR=str(directory))

    def accepts(self, source, flags=(), link=False):
        key = (source, tuple(flags), link)
        if key not in self.cache:
            build_flags = (shlex.split(self.environment.get('CPPFLAGS', '')) +
                           shlex.split(self.environment.get('CFLAGS', '')) +
                           shlex.split(self.environment.get('PLATFORM_FLAGS', '')))
            link_flags = shlex.split(self.environment.get('LDFLAGS', '')) if link else []
            source_path = self.directory / 'probe.c'
            write_changed(source_path, source)
            command = [*self.command, *build_flags, '-D_GNU_SOURCE', '-std=c17', *flags,
                       *([] if link else ['-c']), str(source_path), *link_flags,
                       '-o', str(self.directory / ('probe.exe' if link else 'probe.o'))]
            result = subprocess.run(command, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=self.environment)
            self.cache[key] = result.returncode == 0
            with (self.directory / 'probes.log').open('a') as log:
                log.write(source + '\n' + result.stderr + '\n')
        return self.cache[key]

    def values(self, spec):
        values = {}
        available_headers = []
        for name, header in spec.get('headers', {}).items():
            if self.accepts(f'#include <{header}>\n'):
                values[name] = 1
                available_headers.append(header)
        preamble = ''.join(f'#include <{header}>\n' for header in available_headers)
        for name, expression in spec.get('expressions', {}).items():
            if self.accepts(preamble + f'void probe(void) {{ (void)({expression}); }}\n'):
                values[name] = 1
        for name, alternatives in spec.get('types', {}).items():
            for typename in alternatives:
                if self.accepts(preamble + f'void probe(void) {{ (void)sizeof({typename}); }}\n'):
                    values[name] = typename
                    break
        for name, typename in spec.get('sizes', {}).items():
            for size in (1, 2, 4, 8, 16):
                if self.accepts(preamble + f'_Static_assert(sizeof({typename}) == {size}, "size");\n'):
                    values[name] = size
                    break
        return values


def render_template(template, values):
    # Optional features are disabled unless selected by JSON or a compiler probe.
    def define(match):
        binary, name, tail = match.groups()
        value = values.get(name)
        if binary:
            return f'#define {name} {int(bool(value))}'
        return f'#define {name}{tail}' if value else f'/* #undef {name} */'
    template = re.sub(r'^\s*#cmakedefine(01)?\s+(\w+)([^\n]*)', define, template, flags=re.M)
    def substitute(match):
        name = match.group(1)
        if name not in values:
            raise ValueError(f'missing template variable {name}')
        return str(values[name])
    return re.sub(r'@([A-Za-z_][A-Za-z_0-9]*)@', substitute, template)


def generate_headers(recipe, variables, probe):
    values = dict(recipe.get('variables', {}))
    values.update(variables)
    values.update(probe.values(recipe.get('probes', {})))
    for spec in recipe.get('metadata', []):
        content = Path(spec['source']).read_text()
        match = re.search(spec['pattern'], content, re.M)
        if not match:
            raise ValueError(f'missing upstream metadata: {spec["name"]}')
        value = match.group(1)
        if spec.get('transform') == 'hex_version':
            major, minor, patch = map(int, value.split('.'))
            value = hex((major << 16) | (minor << 8) | patch)
        values[spec['name']] = value
    for spec in recipe.get('generated_files', []):
        spec = copy.deepcopy(spec)
        resolve_source_sets(spec, variables['system'], variables['architecture'])
        local = dict(values, **spec.get('variables', {}))
        if 'template' in spec:
            text = render_template(Path(spec['template']).read_text(), local)
        else:
            defines = spec.get('defines', {})
            text = '/* generated from dependency configuration; do not edit. */\n#pragma once\n'
            text += ''.join(f'#define {key} {value}\n' for key, value in defines.items())
        write_changed(spec['path'], text)


def make_variable(directory, name, variables=None, makefile='Makefile', query_directory=None):
    # macOS ships Make 3.81, which has no --eval; expand through an included makefile.
    query = Path(query_directory or directory) / 'lambda-variable.mk'
    included = str(Path(directory) / makefile).replace(' ', '\\ ')
    write_changed(query, 'include ' + included + '\n$(info $(' + name + '))\n.PHONY: lambda-build-variable\nlambda-build-variable:\n')
    result = subprocess.run(['make', '--no-print-directory', '-s', '-C', str(directory),
                             '-f', str(query), *[key + '=' + value for key, value in (variables or {}).items()],
                             'lambda-build-variable'],
                            check=True, stdout=subprocess.PIPE, text=True)
    return shlex.split(result.stdout.strip())


def projects_for(recipe, variables):
    projects = []
    for item in recipe['projects']:
        target = copy.deepcopy(item)
        resolve_source_sets(target, variables['system'], variables['architecture'])
        sources = list(target.pop('source_files', []))
        for pattern in target.pop('source_patterns', []):
            matches = sorted(glob.glob(pattern, recursive=True))
            if not matches:
                raise ValueError(f'no dependency sources match {pattern}')
            sources.extend(matches)
        if 'make_sources' in target:
            selection = target.pop('make_sources')
            for name in selection.get('variables', [selection.get('variable', 'OBJS')]):
                entries = make_variable(selection['directory'], name, selection.get('make_variables'),
                                        selection.get('makefile', 'Makefile'), variables['build'])
                for obj in entries:
                    filename = obj if obj.endswith(('.c', '.cc', '.cpp')) else obj.removesuffix('.o') + '.c'
                    sources.append(str(Path(selection['source_directory']) / filename))
            # configure selects the CPU kernel and flags together; preserve that contract.
            for pattern, variable in selection.get('file_flags', {}).items():
                flags = make_variable(selection['directory'], variable, selection.get('make_variables'))
                target.setdefault('file_rules', []).append({'files': [pattern], 'build_options': flags})
        excluded = set(target.pop('exclude_source_files', []))
        target['source_files'] = list(dict.fromkeys(path for path in sources if path not in excluded))
        if not target['source_files'] or any(not Path(path).is_file() for path in target['source_files']):
            raise ValueError(f'missing source in dependency project {target["name"]}')
        target.setdefault('target_dir', variables['build'] + '/lib')
        target.setdefault('obj_dir', variables['build'] + '/obj/%{prj.name}/%{cfg.buildcfg}')
        target.setdefault('lang', 'c17')
        target.setdefault('single_project', True)
        target.setdefault('pic', True)
        target.setdefault('link', 'static')
        projects.append(target)
    return projects


def generate_premake(recipe, variables, projects, compiler, output):
    """Generate the shared dependency workspace from resolved JSON projects."""
    system = variables['system']
    toolset = 'clang' if 'clang' in compiler or system in ('macos', 'windows') else 'gcc'
    def quoted(value):
        return json.dumps(value, ensure_ascii=False)

    lines = [
        '-- generated from dependency build configuration; do not edit',
        'workspace "LambdaDependencies"',
        '    configurations { ' + quoted(variables.get('configuration', 'release')) + ' }',
        '    platforms { "native" }',
        '    location ' + quoted(variables['build'] + '/make'),
        '    toolset ' + quoted(toolset),
        '    cppdialect "C++17"', '    cdialect "C17"', '    warnings "Default"',
    ]

    def emit_list(directive, values, indent='    '):
        if values:
            if directive == 'defines':
                # JSON stores escaped header-name quotes; Premake supplies the shell escaping.
                values = [value.replace('\\"', '"') for value in values]
            lines.append(indent + directive + ' { ' + ', '.join(quoted(v) for v in values) + ' }')

    if variables.get('workspace_optimize', 'Speed'):
        lines.append('    optimize ' + quoted(variables.get('workspace_optimize', 'Speed')))
    emit_list('defines', variables.get('workspace_defines', ['NDEBUG']))
    for project in projects:
        executable = project.get('link') == 'executable'
        language = 'C++' if project.get('lang', 'c17').startswith('c++') else 'C'
        lines.extend(['project ' + quoted(project['name']),
                      '    kind ' + quoted('ConsoleApp' if executable else 'StaticLib'),
                      '    language ' + quoted(language),
                      '    targetdir ' + quoted(project['target_dir']),
                      '    objdir ' + quoted(project['obj_dir'])])
        if project.get('target_name'):
            lines.append('    targetname ' + quoted(project['target_name']))
        if executable:
            lines.append('    targetextension ' + quoted(project.get('target_extension', '.exe')))
        elif system != 'windows':
            # ar retains removed members unless the archive is recreated.
            lines.append("    prelinkcommands { '{DELETE} \"%{cfg.buildtarget.abspath}\"' }")
        emit_list('files', project['source_files'])
        emit_list('includedirs', project.get('includes', []))
        emit_list('defines', project.get('defines', []))
        if project.get('optimize'):
            lines.append('    optimize ' + quoted(project['optimize']))
        options = (['-pedantic'] + (['-fms-extensions'] if toolset == 'clang' else []) +
                   ['-fdiagnostics-color=auto'] + list(recipe.get('flags', [])) + variables.get('platform_flags', []) +
                   list(project.get('build_options', [])))
        if project.get('pic') and '-fPIC' not in options:
            options.append('-fPIC')
        emit_list('buildoptions', options)
        emit_list('links', project.get('libraries', []))
        emit_list('linkoptions', project.get('link_options', []))
        for rule in project.get('file_rules', []):
            for pattern in rule['files']:
                # Premake matches file filters relative to the generated Lua file.
                if os.path.isabs(pattern):
                    pattern = os.path.relpath(pattern, Path(output).parent)
                lines.append('    filter ' + quoted('files:' + pattern))
                for key, directive in (('defines', 'defines'), ('build_options', 'buildoptions'),
                                       ('includes', 'includedirs')):
                    emit_list(directive, rule.get(key, []), '        ')
        lines.append('    filter {}')
    write_changed(output, '\n'.join(lines) + '\n')


def reset_build_inputs(stamp, identity, outputs):
    """Discard only owned outputs when their compiler or source recipe changes."""
    stamp = Path(stamp)
    if stamp.is_file() and stamp.read_text() == identity:
        return False
    for output in map(Path, outputs):
        if output.is_dir() and not output.is_symlink():
            shutil.rmtree(output)
        else:
            output.unlink(missing_ok=True)
    write_changed(stamp, identity)
    return True


def install(recipe, projects, prefix):
    for project in projects:
        if 'install_archive' not in project and 'install_executable' not in project:
            continue
        name = project.get('target_name', project['name'])
        if 'install_archive' in project:
            filename, destination = 'lib' + name + '.a', project['install_archive']
        else:
            filename, destination = name + project.get('target_extension', '.exe'), project['install_executable']
        source = Path(project['target_dir']) / filename
        destination = prefix / destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for spec in recipe.get('install_files', []):
        matches = sorted(glob.glob(spec['source']))
        if not matches:
            raise ValueError(f'no install files match {spec["source"]}')
        for source in matches:
            destination = prefix / spec['destination']
            if spec.get('directory', False):
                destination.mkdir(parents=True, exist_ok=True)
                destination /= Path(source).name
            elif Path(source).is_file():
                destination.parent.mkdir(parents=True, exist_ok=True)
            if Path(source).is_dir():
                shutil.copytree(source, destination, dirs_exist_ok=True)
            else:
                shutil.copy2(source, destination)


def main(root=ROOT, config_path=CONFIG, input_root=None):
    root = Path(root).resolve()
    input_root = Path(input_root or root).resolve()
    config = load_build_config(config_path)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dependency', choices=sorted(config['dependency_builds']))
    parser.add_argument('--source', type=Path)
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--libdir', default='lib')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--install-only', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    system = {'Darwin': 'macos', 'Linux': 'linux', 'Windows': 'windows'}.get(platform.system(), 'windows')
    machine = platform.machine().lower()
    architecture = {'aarch64': 'arm64', 'amd64': 'x86_64', 'x64': 'x86_64'}.get(machine, machine)
    original = config['dependency_builds'][args.dependency]
    source_dir = expand(original['source_dir'], {'root': root, 'input_root': input_root})
    source = (args.source or root / source_dir).resolve()
    if not source.is_dir():
        raise RuntimeError(f'dependency source missing: {source}; run the platform setup script')
    build = root / 'temp' / 'dependency-build' / (system + '-' + architecture) / args.dependency
    build.mkdir(parents=True, exist_ok=True)
    prefix = (args.prefix or build / 'install').resolve()
    variables = {'root': str(root), 'input_root': str(input_root), 'python': sys.executable,
                 'source': str(source), 'build': str(build), 'prefix': str(prefix),
                 'system': system, 'architecture': architecture,
                 'platform_flags': shlex.split(os.environ.get('PLATFORM_FLAGS', '')),
                 'system_name': {'macos': 'Darwin', 'linux': 'Linux', 'windows': 'Windows'}[system]}
    recipe = copy.deepcopy(original)
    for key, value in recipe.pop('platforms', {}).get(system, {}).items():
        if isinstance(value, list):
            recipe.setdefault(key, []).extend(value)
        elif isinstance(value, dict):
            recipe.setdefault(key, {}).update(value)
        else:
            recipe[key] = value
    for query in recipe.pop('queries', []):
        executable = shutil.which(query['tool'])
        if not executable:
            executable = next((str(Path(path) / query['tool']) for path in query.get('search_paths', [])
                               if (Path(path) / query['tool']).is_file()), None)
        if not executable:
            raise RuntimeError(f'missing dependency tool: {query["tool"]}')
        variables[query['name']] = subprocess.check_output([executable, *query['args']], text=True).strip()
    recipe = expand(recipe, variables)
    compiler = os.environ.get('CC', 'clang' if system in ('macos', 'windows') else 'gcc')
    cxx_compiler = os.environ.get('CXX', 'clang++' if system in ('macos', 'windows') else 'g++')
    env = dict(os.environ, TMPDIR=str(build), CC=compiler)
    if system == 'macos':
        env['MACOSX_DEPLOYMENT_TARGET'] = os.environ.get('LAMBDA_MACOS_DEPLOYMENT_TARGET') or \
            os.environ.get('MACOSX_DEPLOYMENT_TARGET') or config['dependency_defaults']['macos']['deployment_target']
    compiler_version = subprocess.check_output([*shlex.split(compiler), '--version'], text=True)
    cxx_version = subprocess.check_output([*shlex.split(cxx_compiler), '--version'], text=True)
    enabled_features = []
    if not args.install_only:
        for index, command in enumerate(recipe.get('prepare', [])):
            Path(command.get('directory', source)).mkdir(parents=True, exist_ok=True)
            stamp = build / f'prepare-{index}.json'
            fingerprint = None
            if command.get('cache'):
                # configure results belong to the compiler, source script and exact option set.
                executable = Path(command['command'][0])
                inputs = {'command': command, 'compiler': compiler,
                          'flags': {key: env.get(key, '') for key in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'AR', 'MACOSX_DEPLOYMENT_TARGET')},
                          'script': hashlib.sha256(executable.read_bytes()).hexdigest(),
                          'compiler_version': compiler_version}
                fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
                if stamp.is_file() and stamp.read_text() == fingerprint and \
                        all(Path(path).is_file() for path in command.get('outputs', [])):
                    continue
            run(command['command'], build / f'prepare-{index}.log', cwd=command.get('directory', source), env=env)
            if fingerprint:
                write_changed(stamp, fingerprint)
        probe = CompilerProbe(compiler, build, env)
        generate_headers(recipe, variables, probe)
        for feature in recipe.get('compiler_features', []):
            if probe.accepts(feature['source'], feature.get('flags', []), link=True):
                enabled_features.append(feature['name'])
                for project in recipe['projects']:
                    project.setdefault('file_rules', []).extend(feature.get('file_rules', []))
    if args.install_only:
        manifest = json.loads((build / 'manifest.json').read_text())
        projects = manifest['projects']
        enabled_features = manifest.get('enabled_features', [])
    else:
        projects = projects_for(recipe, variables)
    for project in projects:
        if 'install_archive' in project:
            project['install_archive'] = str(Path(args.libdir) / Path(project['install_archive']).name)
    if not args.install_only:
        premake = os.environ.get('PREMAKE5_BIN') or shutil.which('premake5')
        if not premake:
            raise RuntimeError('premake5 is missing')
        premake_version = subprocess.check_output([premake, '--version'], text=True)
        identity = json.dumps({'recipe': recipe, 'projects': projects, 'compiler': compiler, 'compiler_version': compiler_version,
                               'cxx_compiler': cxx_compiler, 'cxx_version': cxx_version,
                               'premake_version': premake_version, 'platform_flags': variables['platform_flags'],
                               'builder': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                               'environment': {key: env.get(key, '') for key in
                                               ('CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'AR', 'RANLIB', 'MACOSX_DEPLOYMENT_TARGET')}},
                              sort_keys=True)
        stamp = build / 'build-inputs.json'
        # gmake tracks headers, but cannot detect a changed compiler or flag recipe.
        reset_build_inputs(stamp, identity, (build / 'obj', build / 'lib'))
        script = build / 'premake5.lua'
        generate_premake(recipe, variables, projects, compiler, script)
        # each recipe has its own make directory and object directory, so setup builds cannot collide.
        run([premake, '--file=' + str(script), 'gmake'], build / 'premake.log', cwd=root, env=env)
        if args.generate_only:
            print(f'dependency-build: generated {args.dependency}: {len(projects)} projects')
            return
        command = ['make', '-C', str(build / 'make'), 'config=release_native', '-j' + str(args.jobs),
                   'CC=' + compiler, 'CXX=' + cxx_compiler]
        for name in ('AR', 'RANLIB'):
            if name in os.environ:
                command.append(name + '=' + os.environ[name])
        run(command, build / 'build.log', cwd=root, env=env)
    if not args.build_only:
        install(recipe, projects, prefix)
    write_changed(build / 'manifest.json', json.dumps({'dependency': args.dependency, 'source': str(source),
                  'projects': projects, 'prefix': str(prefix), 'enabled_features': enabled_features}, indent=2) + '\n')
    action = f'built {args.dependency}' if args.build_only else f'installed {args.dependency} in {prefix}'
    print(f'dependency-build: {action}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f'dependency-build: {error}', file=sys.stderr)
        sys.exit(1)
