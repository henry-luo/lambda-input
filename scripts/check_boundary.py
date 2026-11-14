#!/usr/bin/env python3
"""Reject first-party compiler dependencies that escape through migration links."""
import os
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
profile = sys.argv[1] if len(sys.argv) > 1 else 'debug'
if profile not in ('debug', 'release'):
    raise SystemExit(f'unsupported build profile: {profile}')
errors = []
for target, group in (('lambda-lib', 'lib'), ('lambda-data', 'data')):
    sources = subprocess.check_output([sys.executable, str(root / 'scripts/build_sources.py'), target], text=True).split()
    for source in sources:
        depfile = root / 'build/obj' / group / profile / (source + '.o.d')
        if not depfile.is_file():
            errors.append(f'missing dependency file for {source}')
            continue
        rule = depfile.read_text().replace('\\\n', ' ').splitlines()[0]
        _, separator, dependencies = rule.partition(': ')
        if not separator:
            errors.append(f'invalid dependency file {depfile}')
            continue
        for name in shlex.split(dependencies):
            lexical = Path(os.path.abspath(root / name))
            # Explicit dependency include directories may live outside the source tree.
            if not lexical.is_relative_to(root):
                continue
            physical = lexical.resolve()
            if not physical.is_relative_to(root):
                errors.append(f'{source}: {lexical.relative_to(root)} resolves outside lambda-input: {physical}')
if errors:
    print('\n'.join(sorted(set(errors))), file=sys.stderr)
    raise SystemExit(1)
print('INPUT_BOUNDARY: all archive source and header dependencies remain in lambda-input')
