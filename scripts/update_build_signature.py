#!/usr/bin/env python3
"""Invalidate input objects when their compiler or dependency paths change."""
import json
import os
from pathlib import Path
import sys
import time

from generate_image_config import write_changed

kind = sys.argv[1] if len(sys.argv) == 2 else 'firstparty'
if kind == 'firstparty':
    names = ('CC', 'CXX', 'AR', 'CPPFLAGS', 'CFLAGS', 'CXXFLAGS', 'PLATFORM_FLAGS',
             'INPUT_DEPS_ROOT', 'INPUT_CPPFLAGS', 'LIB_DEFINES', 'DATA_DEFINES',
             'LIB_SOURCES', 'DATA_SOURCES')
    roots = ('build/obj/lib', 'build/obj/data')
elif kind == 'radiant':
    names = ('CXX', 'AR', 'CPPFLAGS', 'CXXFLAGS', 'PLATFORM_FLAGS', 'RADIANT_CPPFLAGS',
             'DEFINES', 'NATIVE_SOURCES', 'PROPERTY_SOURCES', 'OBJCXX_SOURCES')
    roots = ('build/obj/native', 'build/obj/properties')
elif kind == 'native':
    names = ('CC', 'CXX', 'AR', 'CPPFLAGS', 'PLATFORM_FLAGS', 'NATIVE_FLAGS', 'NATIVE_PROFILE')
    roots = ('build/obj/native-deps',)
else:
    raise SystemExit(f'input build: unknown signature kind {kind}')
values = {name: os.environ.get(name, '') for name in names}
path = Path('build/' + kind + '-build.json')
content = json.dumps(values, sort_keys=True, indent=2) + '\n'
if not path.exists() or path.read_text() != content:
    # macOS Make 3.81 compares whole seconds; a same-second config switch
    # must be newer than every object, including one built in the last command.
    objects = (item for root in roots for item in Path(root).rglob('*.o'))
    newest = max((int(item.stat().st_mtime) for item in objects), default=0)
    delay = newest + 1 - time.time()
    if delay > 2:
        raise SystemExit('input build: object timestamp is in the future; check the clock')
    if delay > 0:
        time.sleep(delay + 0.01)
    write_changed(path, content)
