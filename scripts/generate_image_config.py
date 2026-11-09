#!/usr/bin/env python3
"""Generate codec headers and dispatch tables from the native dependency configuration."""
from pathlib import Path
import subprocess
import sys
from build_config import load_build_config


def write_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text() == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def main():
    settings = load_build_config(sys.argv[1])
    configs = [settings[name] for name in ('image_codecs', 'archive_codecs') if name in settings]
    config = {key: [spec for group in configs for spec in group.get(key, [])]
              for key in ('generated_files', 'dispatch_generators')}
    for spec in config['generated_files']:
        lines = ['/* generated from native dependency configuration; do not edit. */']
        lines += spec.get('preamble', [])
        lines += [f'#define {key} {value}'.rstrip()
                  for key, value in spec.get('defines', {}).items()]
        lines += spec.get('lines', [])
        write_changed(spec['path'], '\n'.join(lines) + '\n')
    for spec in config['dispatch_generators']:
        # Keep the upstream dispatch DSL; feature selection is owned by JSON.
        # Codec sources belong to input even when the caller builds from Lambda.
        input_root = Path(__file__).resolve().parent.parent
        command = [str(input_root / part) if part.startswith('lib/') else part
                   for part in spec['command']]
        output = subprocess.check_output(command, text=True)
        write_changed(spec['path'], output)


if __name__ == '__main__':
    main()
