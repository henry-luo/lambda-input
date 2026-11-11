#!/usr/bin/env python3
"""Run a build command quietly, retaining error diagnostics and its result."""

import argparse
import re
import subprocess
import sys
from pathlib import Path


ERROR_LINE = re.compile(
    r"\b(?:fatal )?error(?:\s+[A-Z]?\d+)?\s*:|"
    r"\b(?:failed|failure)\b|\bundefined symbols?\b|"
    r"\bsymbol(?:s|\(s\))? not found\b|make(?:\[\d+\])?: \*\*\*",
    re.IGNORECASE,
)


def main(argv):
    try:
        separator = argv.index("--")
    except ValueError:
        print("quiet_build: expected -- before the build command", file=sys.stderr)
        return 2

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--log", required=True)
    options = parser.parse_args(argv[:separator])
    command = argv[separator + 1:]
    if not command:
        print("quiet_build: missing build command", file=sys.stderr)
        return 2

    log_path = Path(options.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with log_path.open("w", encoding="utf-8", errors="replace") as log_file:
            result = subprocess.run(command, stdout=log_file, stderr=subprocess.STDOUT, check=False)
    except OSError as error:
        print(f"{options.label}: failed to start build: {error}", file=sys.stderr)
        print(f"❌ Build failed for target: {options.label}", file=sys.stderr)
        return 1

    if result.returncode == 0:
        print(f"✅ Build completed successfully for target: {options.label} (log: {log_path})")
        return 0

    with log_path.open("r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            if ERROR_LINE.search(line):
                print(line, end="" if line.endswith("\n") else "\n", file=sys.stderr)
    print(f"❌ Build failed for target: {options.label} (log: {log_path})", file=sys.stderr)
    return result.returncode if 0 < result.returncode < 256 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
