#!/usr/bin/env python3
"""Fetch the pinned external YAML test corpus used by the input test suite."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
YAML = ROOT / "test/yaml"
REVISION = "6ad3d2c62885d82fc349026c136ef560838fdf3d"
URL = "https://github.com/yaml/yaml-test-suite.git"


def run(*args):
    subprocess.run(args, check=True, cwd=ROOT)


def main():
    if not YAML.exists():
        YAML.mkdir(parents=True)
        run("git", "-C", str(YAML), "init", "-q")
        run("git", "-C", str(YAML), "remote", "add", "origin", URL)
        run("git", "-C", str(YAML), "fetch", "--depth=1", "origin", REVISION)
        run("git", "-C", str(YAML), "checkout", "--detach", "FETCH_HEAD")
    if not (YAML / ".git").exists():
        raise SystemExit("input test data: test/yaml exists but is not the pinned YAML checkout")
    actual = subprocess.check_output(["git", "-C", str(YAML), "rev-parse", "HEAD"], text=True).strip()
    if actual != REVISION:
        raise SystemExit(f"input test data: test/yaml is at {actual}; expected {REVISION}")
    corpus = Path(os.environ.get("LAMBDA_TEST_DIR", ROOT.parent / "lambda-test")).resolve() / "markdown"
    if not corpus.is_dir():
        raise SystemExit(f"input test data: Markdown corpus missing: {corpus}")


if __name__ == "__main__":
    main()
