#!/usr/bin/env python3
"""Expand an archive's owned source manifest."""
import argparse
import fnmatch
import glob
import json
import platform
from pathlib import Path

from build_config import resolve_source_sets


def expand_sources(root, target):
    sources = set(target.get('source_files', ()))
    for pattern in target.get('source_patterns', ()):
        sources.update(str(Path(path).relative_to(root)) for path in glob.glob(str(root / pattern)))
    excludes = target.get('exclude_patterns', ())
    sources = sorted(path for path in sources if not any(fnmatch.fnmatchcase(path, pattern) for pattern in excludes))
    missing = [path for path in sources if not (root / path).is_file()]
    if missing:
        raise ValueError(f"missing {target['name']} sources: {missing}")
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target')
    parser.add_argument('root', nargs='?', default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--objcxx', action='store_true', help='list Objective-C++ sources for the selected platform')
    parser.add_argument('--system', choices=('macos', 'linux', 'windows'))
    parser.add_argument('--architecture')
    args = parser.parse_args()
    root = Path(args.root).resolve()
    system = args.system or {'Darwin': 'macos', 'Linux': 'linux', 'Windows': 'windows'}.get(platform.system())
    if not system:
        raise SystemExit(f'unsupported system: {platform.system()}')
    machine = (args.architecture or platform.machine()).lower()
    architecture = {'aarch64': 'arm64', 'amd64': 'x86_64', 'x64': 'x86_64'}.get(machine, machine)
    config = json.loads((root / 'build_targets.json').read_text())
    target = next(item for item in config['targets'] if item['name'] == args.target)
    resolve_source_sets(target, system, architecture)
    sources = expand_sources(root, target)
    # First-party archive object names use source basenames.
    names = [Path(path).name + '.o' for path in sources]
    if len(names) != len(set(names)):
        raise ValueError(f"duplicate archive member in {target['name']}")
    if args.objcxx:
        sources = sorted(set(sources).intersection(target.get('objcxx_source_files', ())))
    print(' '.join(sources))


if __name__ == '__main__':
    main()
