#!/usr/bin/env python3
"""Probe the host for curl's native configuration without its build system."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import ssl
import subprocess
import sys

from dependency_builder import CompilerProbe
from generate_image_config import write_changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--config', required=True, type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    settings = json.loads(args.config.read_text())['curl_dependency']
    template = source / 'lib/curl_config.h.in'
    build = output.parent.parent
    build.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get('CC') or 'cc'
    env = dict(os.environ, TMPDIR=str(build))
    compiler_version = subprocess.check_output([*shlex.split(compiler), '--version'], text=True)
    bundle = ssl.get_default_verify_paths().openssl_cafile
    identity = {'template': hashlib.sha256(template.read_bytes()).hexdigest(),
                'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'probe': hashlib.sha256(Path(__file__).with_name('dependency_builder.py').read_bytes()).hexdigest(),
                'profile': settings, 'compiler': compiler, 'compiler_version': compiler_version,
                'platform': platform.platform(), 'bundle': bundle,
                'environment': {key: env.get(key, '') for key in
                                ('CPPFLAGS', 'CFLAGS', 'LDFLAGS', 'PLATFORM_FLAGS', 'INPUT_CURL_TOOLCHAIN_ID')}}
    signature = json.dumps(identity, sort_keys=True, indent=2) + '\n'
    stamp = build / 'curl-config-inputs.json'
    if output.is_file() and stamp.is_file() and stamp.read_text() == signature:
        return

    probe = CompilerProbe(compiler, build, env)
    flags = ['-Werror=implicit-function-declaration']
    profile = {name: 1 for name in settings['defines']}
    profile.update({'USE_MBEDTLS': 1, 'USE_NGHTTP2': 1, 'HAVE_BROTLI': 1,
                    'HAVE_LIBBROTLIDEC': 1, 'HAVE_LIBZ': 1})
    for header in ('mbedtls/ssl.h', 'nghttp2/nghttp2.h', 'brotli/decode.h', 'zlib.h'):
        if not probe.accepts(f'#include <{header}>\nint main(void) {{ return 0; }}\n', flags):
            raise RuntimeError(f'curl configuration: required header {header} is unavailable')
    profile['HAVE_BROTLI_DECODE_H'] = 1
    profile['HAVE_NGHTTP2_NGHTTP2_H'] = 1
    if bundle and Path(bundle).is_file():
        profile['CURL_CA_BUNDLE'] = json.dumps(bundle)

    if platform.system() not in ('Darwin', 'Linux'):
        # curl maintains this native Windows feature configuration upstream.
        if not probe.accepts('#include <winsock2.h>\nint main(void) { return AF_INET6 == 0; }\n', flags):
            raise RuntimeError('curl configuration: IPv6 support is unavailable')
        profile['USE_IPV6'] = 1
        lines = ['/* generated from input curl configuration; do not edit. */']
        lines += [f'#define {name} {value}' for name, value in sorted(profile.items())]
        lines += ['#include "config-win32.h"', '#undef USE_UNIX_SOCKETS', '']
        write_changed(output, '\n'.join(lines))
        write_changed(stamp, signature)
        return

    text = template.read_text()
    comments = re.findall(r'/\*\s*(.*?)\s*\*/\s*#undef\s+(HAVE_\w+)\b', text, re.S)
    allowed = set(settings['probe_headers'])
    header_map = {}
    for comment, macro in comments:
        match = re.search(r'<([^>]+)>', comment)
        if macro.endswith('_H') and match and match.group(1) in allowed:
            header_map[match.group(1)] = macro
    # Extra headers provide declarations for probes; curl has no corresponding macro.
    headers = list(settings['probe_headers'])
    prelude = ''
    available = []
    for header in headers:
        test = prelude + f'#include <{header}>\nint main(void) {{ return 0; }}\n'
        if probe.accepts(test, flags):
            prelude += f'#include <{header}>\n'
            available.append(header)
            if header in header_map:
                profile[header_map[header]] = 1

    # A few functions need separate signature checks before curl can use them.
    signature_functions = {'fsetxattr', 'gethostbyname_r', 'strerror_r', 'sigsetjmp'}
    for comment, macro in comments:
        match = re.search(r'(?:the\s+)?[`\']?([A-Za-z_]\w*)[\'`]?\s+function', comment)
        if not match:
            continue
        symbol = match.group(1)
        if macro != 'HAVE_' + symbol.upper() or symbol in signature_functions:
            continue
        body = f'void (* volatile probe_symbol)(void) = (void (*)(void)){symbol};\nint main(void) {{ return probe_symbol == 0; }}\n'
        if probe.accepts(prelude + body, flags, link=True):
            profile[macro] = 1

    def check(name, body, extra=flags):
        if probe.accepts(prelude + body + '\n', extra, link=True):
            profile[name] = 1

    check('HAVE_OPENDIR', 'int main(void) { return opendir(".") == 0; }')
    if platform.system() == 'Darwin':
        check('HAVE_BUILTIN_AVAILABLE',
              'int main(void) { if (__builtin_available(macOS 10.12, *)) return 0; return 1; }')
    for macro, expression in {
        'HAVE_ATOMIC': '_Atomic int v = 0; (void)v',
        'HAVE_BOOL_T': '_Bool v = 0; (void)v',
        'HAVE_LONGLONG': 'long long v = 0; (void)v',
        'HAVE_SUSECONDS_T': 'suseconds_t v = 0; (void)v',
        'HAVE_STRUCT_SOCKADDR_STORAGE': 'struct sockaddr_storage v; (void)v',
        'HAVE_STRUCT_TIMEVAL': 'struct timeval v; (void)v',
        'HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID': 'struct sockaddr_in6 v; (void)v.sin6_scope_id',
        'HAVE_FCNTL_O_NONBLOCK': '(void)O_NONBLOCK',
        'HAVE_IOCTL_FIONBIO': '(void)FIONBIO',
        'HAVE_IOCTL_SIOCGIFADDR': '(void)SIOCGIFADDR',
        'HAVE_MSG_NOSIGNAL': '(void)MSG_NOSIGNAL',
        'HAVE_CLOCK_GETTIME_MONOTONIC': '(void)CLOCK_MONOTONIC',
        'HAVE_CLOCK_GETTIME_MONOTONIC_RAW': '(void)CLOCK_MONOTONIC_RAW',
        'USE_IPV6': '(void)AF_INET6; struct sockaddr_in6 v; (void)v',
    }.items():
        check(macro, f'int main(void) {{ {expression}; return 0; }}')
    if probe.accepts(prelude + 'int main(void) { sigjmp_buf b; return sigsetjmp(b, 1); }\n', flags, link=True):
        profile['HAVE_SIGSETJMP'] = 1
    for count, call in ((5, 'fsetxattr(-1, "", "", 0, 0)'),
                        (6, 'fsetxattr(-1, "", "", 0, 0, 0)')):
        if probe.accepts(prelude + f'int main(void) {{ return {call}; }}\n', flags, link=True):
            profile['HAVE_FSETXATTR'] = 1
            profile[f'HAVE_FSETXATTR_{count}'] = 1
            break
    strict = flags + ['-Werror=incompatible-pointer-types', '-Werror=int-conversion']
    for name, declaration in (('HAVE_POSIX_STRERROR_R', 'int (*f)(int, char *, size_t) = &strerror_r'),
                              ('HAVE_GLIBC_STRERROR_R', 'char *(*f)(int, char *, size_t) = &strerror_r')):
        if probe.accepts(prelude + f'int main(void) {{ {declaration}; return f == 0; }}\n', strict, link=True):
            profile['HAVE_STRERROR_R'] = 1
            profile[name] = 1
            break
    if profile.get('HAVE_FSEEKO'):
        profile['HAVE_DECL_FSEEKO'] = 1
    if profile.get('HAVE_GETPWUID_R'):
        profile['HAVE_DECL_GETPWUID_R'] = 1
    if profile.get('HAVE_GETADDRINFO'):
        try:
            if os.sysconf('SC_THREAD_SAFE_FUNCTIONS') > 0:
                profile['HAVE_GETADDRINFO_THREADSAFE'] = 1
        except (OSError, ValueError):
            pass
    if 'pthread.h' in available and probe.accepts(prelude + 'int main(void) { pthread_t t = pthread_self(); (void)t; return 0; }\n', ['-pthread'], link=True):
        profile['USE_THREADS_POSIX'] = 1
    required = ('HAVE_GETADDRINFO', 'HAVE_RECV', 'HAVE_SEND', 'HAVE_SOCKET', 'USE_IPV6', 'USE_THREADS_POSIX')
    missing = [name for name in required if name not in profile]
    if missing:
        raise RuntimeError('curl configuration: missing required host features: ' + ', '.join(missing))
    profile['STDC_HEADERS'] = 1
    profile['OS'] = json.dumps(platform.machine() + '-' + platform.system().lower())
    if probe.accepts(prelude + 'int main(void) { sa_family_t v = 0; return v; }\n', flags):
        profile['CURL_SA_FAMILY_T'] = 'sa_family_t'
    sizes = {'SIZEOF_INT': 'int', 'SIZEOF_LONG': 'long', 'SIZEOF_LONG_LONG': 'long long',
             'SIZEOF_OFF_T': 'off_t', 'SIZEOF_SIZE_T': 'size_t', 'SIZEOF_TIME_T': 'time_t',
             'SIZEOF_CURL_OFF_T': 'curl_off_t', 'SIZEOF_CURL_SOCKET_T': 'curl_socket_t'}
    for name, typename in sizes.items():
        for size in (1, 2, 4, 8, 16):
            code = prelude + '#include <curl/curl.h>\n' + f'_Static_assert(sizeof({typename}) == {size}, "size");\n'
            if probe.accepts(code):
                profile[name] = size
                break
        if name not in profile:
            raise RuntimeError(f'curl configuration: cannot determine sizeof({typename})')
    def replace(match):
        name = match.group(1)
        return f'#define {name} {profile[name]}' if name in profile else f'/* #undef {name} */'
    output_text = re.sub(r'^#undef[ \t]+(\w+)[ \t]*$', replace, text, flags=re.M)
    write_changed(output, output_text)
    write_changed(stamp, signature)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'input curl configuration: {error}', file=sys.stderr)
        sys.exit(1)
