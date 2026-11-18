#!/usr/bin/env python3
"""Prepare shared corpora used by the input test suites."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS_ROOT = Path(os.environ.get("LAMBDA_TEST_DIR", ROOT.parent / "lambda-test")).resolve()
YAML = CORPUS_ROOT / "yaml"
REVISION = "6ad3d2c62885d82fc349026c136ef560838fdf3d"


def main():
    if not CORPUS_ROOT.is_dir():
        raise SystemExit(f"input test data: shared corpus repository missing: {CORPUS_ROOT}")
    revision_file = YAML / "UPSTREAM_REVISION"
    if not revision_file.is_file():
        raise SystemExit(f"input test data: YAML corpus missing: {YAML}")
    actual = revision_file.read_text(encoding="utf-8").strip()
    if actual != REVISION:
        raise SystemExit(f"input test data: YAML corpus is at {actual}; expected {REVISION}")
    corpus = CORPUS_ROOT / "markdown"
    if not corpus.is_dir():
        raise SystemExit(f"input test data: Markdown corpus missing: {corpus}")


if __name__ == "__main__":
    main()
