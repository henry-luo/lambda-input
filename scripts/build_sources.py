#!/usr/bin/env python3
"""Expand the input archive source manifest for the standalone make build."""
import fnmatch
import glob
import json
from pathlib import Path
import sys

root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent
config = json.loads((root / 'build_targets.json').read_text())
name = sys.argv[1]
target = next(item for item in config['targets'] if item['name'] == name)
sources = set(target.get('source_files', ()))
for pattern in target.get('source_patterns', ()):
    sources.update(str(Path(path).relative_to(root)) for path in glob.glob(str(root / pattern)))
excludes = target.get('exclude_patterns', ())
sources = sorted(path for path in sources if not any(fnmatch.fnmatchcase(path, pattern) for pattern in excludes))
missing = [path for path in sources if not (root / path).is_file()]
if missing:
    raise SystemExit(f'missing {name} sources: {missing}')
# ar stores basenames, so two source names must not claim the same member.
names = [Path(path).name + '.o' for path in sources]
if len(names) != len(set(names)):
    raise SystemExit(f'duplicate archive member in {name}')
print(' '.join(sources))
