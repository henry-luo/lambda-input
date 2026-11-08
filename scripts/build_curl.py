#!/usr/bin/env python3
"""Build the input-owned curl with the existing mbedTLS networking profile."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / 'build_native_deps_config.json'
DEPS = ROOT / 'build' / 'deps'


def output(command):
    return subprocess.check_output(command, text=True).strip()


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


def main():
    if platform.system() != 'Darwin':
        raise SystemExit('input curl: source build is currently supported on macOS only')
    config = json.loads(CONFIG.read_text())['curl_dependency']
    pkg_config = shlex.split(os.environ.get('PKG_CONFIG', 'pkg-config'))
    packages = ('mbedtls', 'libnghttp2')
    prefixes = {name: output([*pkg_config, '--variable=prefix', name]) for name in packages}
    versions = {name: output([*pkg_config, '--modversion', name]) for name in packages}
    variables = {key: os.environ.get(key, '') for key in
                 ('CC', 'CXX', 'AR', 'CPPFLAGS', 'CFLAGS', 'LDFLAGS', 'PLATFORM_FLAGS')}
    compiler = shlex.split(variables['CC'] or 'cc')
    compiler_version = output([*compiler, '--version']).splitlines()[0]
    script_digest = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    signature = json.dumps({'config': config, 'prefixes': prefixes, 'versions': versions,
                            'variables': variables, 'compiler_version': compiler_version,
                            'script': script_digest}, sort_keys=True, indent=2) + '\n'
    prefix = DEPS / 'curl'
    stamp = DEPS / 'curl-build.json'
    if stamp.is_file() and stamp.read_text() == signature and validate(prefix, config):
        print(f'INPUT_CURL: {config["version"]} mbedTLS already built at {prefix}', flush=True)
        return

    version = config['version']
    archive = DEPS / 'downloads' / f'curl-{version}.tar.xz'
    if not archive.is_file():
        archive.parent.mkdir(parents=True, exist_ok=True)
        partial = archive.with_suffix(archive.suffix + '.part')
        print(f'INPUT_CURL: downloading {config["url"]}', flush=True)
        with urllib.request.urlopen(config['url'], timeout=60) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out)
        partial.replace(archive)
    digest = hashlib.sha256()
    with archive.open('rb') as downloaded:
        for chunk in iter(lambda: downloaded.read(1024 * 1024), b''):
            digest.update(chunk)
    digest = digest.hexdigest()
    if digest != config['sha256']:
        raise SystemExit(f'input curl: checksum mismatch for {archive}; remove the invalid download')

    source = DEPS / 'source' / f'curl-{version}'
    source_stamp = source.parent / f'curl-{version}.sha256'
    if (not (source / 'configure').is_file() or not source_stamp.is_file() or
            source_stamp.read_text().strip() != digest):
        source.parent.mkdir(parents=True, exist_ok=True)
        # An interrupted extraction must not be mistaken for a complete source tree.
        if source.exists():
            shutil.rmtree(source)
        subprocess.run(['tar', '-xJf', str(archive), '-C', str(source.parent)], check=True)
        source_stamp.write_text(digest + '\n')
    build = DEPS / 'curl-build'
    # These directories contain only this script's generated dependency build.
    for path in (build, prefix):
        if path.exists():
            shutil.rmtree(path)
    build.mkdir(parents=True)
    env = os.environ.copy()
    platform_flags = variables['PLATFORM_FLAGS']
    for key in ('CFLAGS', 'LDFLAGS'):
        env[key] = ' '.join(value for value in (variables[key], platform_flags) if value)
    command = [str(source / 'configure'), f'--prefix={prefix}',
               f'--with-mbedtls={prefixes["mbedtls"]}', f'--with-nghttp2={prefixes["libnghttp2"]}',
               *config['configure_options']]
    print(f'INPUT_CURL: building {version} with mbedTLS and HTTP/2', flush=True)
    subprocess.run(command, cwd=build, env=env, check=True)
    jobs = int(os.environ.get('CURL_JOBS', min(8, os.cpu_count() or 1)))
    if jobs < 1:
        raise SystemExit('input curl: CURL_JOBS must be positive')
    # The input provider needs the library, public headers and curl-config,
    # not curl's executable or generated man pages.
    subprocess.run(['make', '-C', 'lib', f'-j{jobs}'], cwd=build, env=env, check=True)
    for directory in ('lib', 'include'):
        subprocess.run(['make', '-C', directory, 'install'], cwd=build, env=env, check=True)
    subprocess.run(['make', 'install-binSCRIPTS', 'install-pkgconfigDATA'],
                   cwd=build, env=env, check=True)
    if not validate(prefix, config):
        raise SystemExit('input curl: built archive does not have the required backend and features')
    stamp.write_text(signature)
    print(f'INPUT_CURL: {version} mbedTLS built at {prefix}', flush=True)


if __name__ == '__main__':
    main()
