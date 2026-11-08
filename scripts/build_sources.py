#!/usr/bin/env python3
"""Expand an archive's owned source manifest."""
import fnmatch
import glob
import json
from pathlib import Path
import sys


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
    root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent
    config = json.loads((root / 'build_targets.json').read_text())
    target = next(item for item in config['targets'] if item['name'] == sys.argv[1])
    sources = expand_sources(root, target)
    # First-party archive object names use source basenames.
    names = [Path(path).name + '.o' for path in sources]
    if len(names) != len(set(names)):
        raise ValueError(f"duplicate archive member in {target['name']}")
    print(' '.join(sources))


if __name__ == '__main__':
    main()
