#!/usr/bin/env python3
"""Build the input-owned curl with the configured mbedTLS networking profile."""
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
import tarfile
import tempfile
import urllib.request

from generate_image_config import write_changed

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / 'build_native_deps_config.json'
DEPS = ROOT / 'build/deps'


def output(command, env=None):
    return subprocess.check_output(command, text=True, env=env).strip()


def validate(prefix, config):
    tool = prefix / 'bin/curl-config'
    if (not tool.is_file() or not (prefix / 'lib/libcurl.a').is_file() or
            not (prefix / 'include/curl/curl.h').is_file()):
        return False
    try:
        features = set(output([str(tool), '--features']).splitlines())
        protocols = set(output([str(tool), '--protocols']).splitlines())
        return (output([str(tool), '--version']) == f"libcurl {config['version']}" and
                output([str(tool), '--ssl-backends']) == 'mbedTLS' and
                set(config['required_features']).issubset(features) and
                set(config['required_protocols']).issubset(protocols))
    except (OSError, subprocess.CalledProcessError):
        return False


def verify_source(source, archive, version):
    # The checksum stamp records extraction; it cannot prove the tree stayed unchanged.
    with tarfile.open(archive, 'r:xz') as tar:
        for member in tar:
            if not member.isfile():
                continue
            parts = Path(member.name).parts
            if not parts or parts[0] != f'curl-{version}' or '..' in parts:
                raise ValueError('input curl: unexpected path in source archive')
            path = source.joinpath(*parts[1:])
            if not path.is_file() or path.stat().st_size != member.size:
                raise ValueError(f'input curl: source differs from pinned archive: {path}')
            with tar.extractfile(member) as expected, path.open('rb') as actual:
                for chunk in iter(lambda: expected.read(1024 * 1024), b''):
                    if actual.read(len(chunk)) != chunk:
                        raise ValueError(f'input curl: source differs from pinned archive: {path}')


def source_tree(config, env):
    version = config['version']
    archive = DEPS / 'downloads' / f'curl-{version}.tar.xz'
    if not archive.is_file():
        archive.parent.mkdir(parents=True, exist_ok=True)
        print(f'INPUT_CURL: downloading {config["url"]}', flush=True)
        partial = None
        try:
            with tempfile.NamedTemporaryFile(prefix='curl-download-', suffix='.part', dir=ROOT / 'temp',
                                             delete=False) as out:
                partial = Path(out.name)
                with urllib.request.urlopen(config['url'], timeout=60) as response:
                    shutil.copyfileobj(response, out)
            partial.replace(archive)
        finally:
            if partial:
                partial.unlink(missing_ok=True)
    digest = hashlib.sha256()
    with archive.open('rb') as downloaded:
        for chunk in iter(lambda: downloaded.read(1024 * 1024), b''):
            digest.update(chunk)
    digest = digest.hexdigest()
    if digest != config['sha256']:
        raise ValueError(f'input curl: checksum mismatch for {archive}')
    source = DEPS / 'source' / f'curl-{version}'
    stamp = source.parent / f'curl-{version}.sha256'
    if (not (source / 'lib/Makefile.inc').is_file() or not stamp.is_file() or
            stamp.read_text().strip() != digest):
        source.parent.mkdir(parents=True, exist_ok=True)
        # A pre-existing unverified tree may contain local work; do not replace it.
        if source.exists():
            raise ValueError(f'input curl: inspect unverified source tree {source}')
        with tempfile.TemporaryDirectory(prefix='curl-source-', dir=ROOT / 'temp') as directory:
            subprocess.run(['tar', '-xJf', str(archive), '-C', directory], env=env, check=True)
            extracted = Path(directory) / f'curl-{version}'
            if not (extracted / 'lib/Makefile.inc').is_file():
                raise ValueError('input curl: archive has no source manifest')
            extracted.rename(source)
        stamp.write_text(digest + '\n')
    verify_source(source, archive, version)
    return source


def runtime_info(prefix, pkg_flags, platform_flags, env):
    scratch = ROOT / 'temp/curl-metadata'
    scratch.mkdir(parents=True, exist_ok=True)
    exe = scratch / ('curl-info.exe' if os.environ.get('MSYSTEM') else 'curl-info')
    command = [*shlex.split(env.get('CC') or 'cc'), *shlex.split(env.get('CFLAGS', '')),
               *shlex.split(env.get('PLATFORM_FLAGS', '')), '-std=c17', '-DCURL_STATICLIB',
               '-I' + str(prefix / 'include'), str(ROOT / 'scripts/curl_info.c'),
               str(prefix / 'lib/libcurl.a'), *pkg_flags, *platform_flags,
               *shlex.split(env.get('LDFLAGS', '')), '-o', str(exe)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    fields = {}
    for line in output([str(exe)], env=env).splitlines():
        key, separator, value = line.partition('=')
        if not separator:
            raise ValueError('input curl: invalid runtime metadata')
        fields.setdefault(key, []).append(value)
    return fields


def render(template, values):
    def replace(match):
        name = match.group(1)
        if name not in values:
            raise ValueError(f'input curl: missing metadata {name}')
        return values[name]
    return re.sub(r'@([A-Za-z_][A-Za-z_0-9]*)@', replace, template)


def install_metadata(source, staging, prefix, config, info, pkg_flags, platform_flags, compiler):
    version = info.get('version', [''])[0]
    ssl_version = info.get('ssl', [''])[0]
    features = set(info.get('feature', []))
    protocols = {item.upper() for item in info.get('protocol', [])}
    # curl-config reports the curl tool's HTTP gateway aliases as protocols.
    for base, aliases in config.get('protocol_aliases', {}).items():
        if base in protocols:
            protocols.update(aliases)
    if (version != config['version'] or not ssl_version.startswith('mbedTLS/') or
            not set(config['required_features']).issubset(features) or
            not set(config['required_protocols']).issubset(protocols)):
        raise ValueError(f'input curl: built archive lacks required features: {info}')
    parts = [int(part) for part in version.split('.')]
    if len(parts) != 3:
        raise ValueError(f'input curl: invalid version {version}')
    number = f'{(parts[0] << 16) | (parts[1] << 8) | parts[2]:06x}'
    backend = ssl_version.split('/', 1)[0]
    common = {'prefix': str(prefix), 'exec_prefix': '${prefix}', 'libdir': '${prefix}/lib',
              'includedir': '${prefix}/include', 'CURLVERSION': version, 'VERSIONNUM': number,
              'ENABLE_SHARED': 'no', 'ENABLE_STATIC': 'yes', 'libext': 'a',
              'SUPPORT_FEATURES': ' '.join(sorted(features)),
              'SUPPORT_PROTOCOLS': ' '.join(sorted(protocols)),
              'LIBCURL_PC_CFLAGS': '-DCURL_STATICLIB', 'LIBCURL_PC_CFLAGS_PRIVATE': '-DCURL_STATICLIB',
              'SSL_BACKENDS': backend, 'CURL_CA_BUNDLE': info.get('ca', [''])[0],
              'CC': compiler, 'LDFLAGS': '', 'CONFIGURE_OPTIONS': "'input Premake/Make recipe'"}
    config_values = dict(common, LIBCURL_PC_LIBS_PRIVATE=shlex.join([*pkg_flags, *platform_flags]))
    # The upstream shell template encloses these fields in quotes.
    config_values['prefix'] = str(prefix).replace('\\', '\\\\').replace('"', '\\"').replace('$', '\\$').replace('`', '\\`')
    for key in ('CC', 'CURL_CA_BUNDLE'):
        config_values[key] = config_values[key].replace("'", "'\\''")
    tool = staging / 'bin/curl-config'
    write_changed(tool, render((source / 'curl-config.in').read_text(), config_values))
    tool.chmod(0o755)
    requires = ', '.join(config['pkg_config_requires'])
    pc_values = dict(common, LIBCURL_PC_REQUIRES=requires, LIBCURL_PC_REQUIRES_PRIVATE='',
                     LIBCURL_PC_LIBS=shlex.join(platform_flags), LIBCURL_PC_LIBS_PRIVATE='')
    write_changed(staging / 'lib/pkgconfig/libcurl.pc', render((source / 'libcurl.pc.in').read_text(), pc_values))


def main():
    system = {'Darwin': 'macos', 'Linux': 'linux'}.get(platform.system())
    if not system:
        if not os.environ.get('MSYSTEM') and platform.system() != 'Windows':
            raise ValueError('input curl: requires macOS, Linux or an MSYS2 toolchain')
        system = 'windows'
    config = json.loads(CONFIG.read_text())['curl_dependency']
    prefix = DEPS / 'curl'
    env = os.environ.copy()
    env['TMPDIR'] = str(ROOT / 'temp/curl-build')
    Path(env['TMPDIR']).mkdir(parents=True, exist_ok=True)
    source = source_tree(config, env)
    env['PKG_CONFIG_PATH'] = os.pathsep.join(filter(None, (str(DEPS / 'brotli/lib/pkgconfig'), env.get('PKG_CONFIG_PATH', ''))))
    pkg_config = shlex.split(env.get('PKG_CONFIG', 'pkg-config'))
    packages = config['pkg_config_requires']
    versions = {name: output([*pkg_config, '--modversion', name], env) for name in packages}
    prefixes = {name: output([*pkg_config, '--variable=prefix', name], env) for name in packages}
    cflags = output([*pkg_config, '--cflags', *packages], env)
    pkg_flags = shlex.split(output([*pkg_config, '--static', '--libs', *packages], env))
    env['CPPFLAGS'] = ' '.join(filter(None, (env.get('CPPFLAGS', ''), '-I' + shlex.quote(str(source / 'include')), cflags)))
    env['INPUT_CURL_TOOLCHAIN_ID'] = json.dumps({'versions': versions, 'prefixes': prefixes, 'pkg_flags': pkg_flags}, sort_keys=True)
    jobs = int(env.get('CURL_JOBS', env.get('JOBS', min(8, os.cpu_count() or 1))))
    if jobs < 1:
        raise ValueError('input curl: CURL_JOBS must be positive')
    print(f'INPUT_CURL: building {config["version"]} with mbedTLS and HTTP/2', flush=True)
    with tempfile.TemporaryDirectory(prefix='curl-install-', dir=ROOT / 'temp') as directory:
        staging = Path(directory)
        subprocess.run([sys.executable, str(ROOT / 'scripts/dependency_builder.py'), 'curl',
                        '--source', str(source), '--prefix', str(staging), '--jobs', str(jobs)],
                       cwd=ROOT, env=env, check=True)
        platform_flags = config['platform_link_flags'][system]
        info = runtime_info(staging, pkg_flags, platform_flags, env)
        install_metadata(source, staging, prefix, config, info, pkg_flags, platform_flags, env.get('CC') or 'cc')
        if not validate(staging, config):
            raise ValueError('input curl: generated metadata does not describe the required archive')
        shutil.copytree(staging, prefix, dirs_exist_ok=True)
    print(f'INPUT_CURL: {config["version"]} mbedTLS built at {prefix}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'input curl: {error}', file=sys.stderr)
        sys.exit(1)
