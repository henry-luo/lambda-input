# Agent Instructions for lambda-input

This file contains owner-specific agent rules. Use [README.md](README.md) for
dependency setup, platform notes, and the complete build and test workflow.

## Ownership and project boundaries

lambda-input owns the shared support library and data-processing sources:
`lib/`, `lambda/core/`, `lambda/io/`, `lambda/input/`,
`lambda/format/`, and `lambda/network/`. The authoritative source and
archive list is `build_targets.json`; dependency recipes and pins are in
`build_vendor_deps_config.json` and `build_native_deps_config.json`.

Under D9.1.1v4 and D9.1.3v2, input must not depend on the Lambda runtime or
Radiant. Preserve the existing allocation and lifetime contracts for
input-owned values. Preserve the precise-rooting contract at host/runtime
boundaries. Do not add runtime imports, allocation fallbacks, or conservative
native-stack GC scanning. D9.1.5 governs the JSON-driven build pipeline.

Do not change Lambda or Radiant implementation, headers, tests, or build files
as part of input work. If an issue belongs to a sibling project, record it in
that owner's ledger: Lambda's `vibe/Lambda_Issue_Ledger.md` or Radiant's
`vibe/Radiant_Issue_Ledger.md`. For input-owned issues, use the
[Lib and Input Issue Ledger](vibe/Lib_Input_Issue_Ledger.md). Move completed records to the
[Lib and Input Fixed Issue Ledger](vibe/Lib_Input_Issue_Ledger%20%28fixed%29.md). Include
observed behavior, reproduction, evidence, and whether it blocks the dependent
task. Mark unconfirmed causes as unconfirmed. If an issue blocks the task,
record it and stop the affected work; continue independent work when possible.

## Rules for changes

1. Find the root cause before changing behavior. Do not hard-code results or
   add workarounds to hide a defect. Keep a failing test visible when the
   cause is not fixed.
2. Write temporary files under this checkout's `./temp/`, never `/tmp`.
3. Follow C17/C++17 as selected by the source and build target. Use the
   project `lib` types for Lambda data and runtime-facing code; do not
   introduce C++ standard-library containers into those paths.
4. Do not use `printf`, `fprintf`, or `std::cout` for debugging. Use
   `log_debug()`, `log_info()`, or `log_error()` from `lib/log.h`, with
   a distinct prefix at the start of each log line.
5. Do not modify `log.conf`.
6. Do not hand-edit generated Premake/Lua files, generated projects, or
   generated dependency configuration. Update the owner JSON manifest or
   dependency configuration, then run the relevant `make` target.
7. Do not patch vendored dependencies, including sources under `lib/re2/`
   and other vendored codec, archive, image, networking, or rendering trees.
   Fix defects in the input-owned integration. If the root cause belongs
   upstream, stop and explain it before proposing an upstream patch.
8. Do not duplicate helpers. Search for an existing helper first. Extract a
   shared helper before adding a third near-identical case; promote a shared
   `static` helper through its module header instead of copying it.
9. Add a concise, lowercase-starting comment for a non-trivial fix when it
   explains why the code is needed. Do not add narration.
10. Preserve test assertions and reference results. For a new `.ls` script in
    a golden-driven test directory, add its expected `.txt` result.
11. Use a release profile for performance measurements.

When a design or implementation record is added or changed, cite the relevant
Lambda formal ruling by `S#` or `D#` when it covers the topic. Use the issue
ledger for issue evidence, not to introduce design rulings.

## Build and verification

The default profile is debug. Read [README.md](README.md) for dependency
prerequisites and platform limitations.

```sh
make                                      # build input and shared libraries
make PROFILE=release                      # build release archives
make test-build-dependencies              # check the shared dependency builder
make test-input-baseline                  # run the input-owned native suites
make test-network-online                  # run tests that contact public endpoints
make PROFILE=release test-perf            # run input performance tests
```

Use focused checks while changing code and the aggregate input baseline at a
milestone or task completion. Dependency-builder changes need
`make test-build-dependencies`. Keep tests that require network access
separate from the local baseline.

## Entry points

| Area | Start here |
|---|---|
| Build ownership | `build_targets.json`, `Makefile`, `scripts/build_sources.py` |
| Dependency builds | `build_vendor_deps_config.json`, `build_native_deps_config.json`, `scripts/` |
| Input dispatch and parsers | `lambda/input/input.cpp`, `lambda/input/` |
| Mark construction and reading | `lambda/io/mark_builder.hpp`, `lambda/core/mark_reader.hpp` |
| Formatters | `lambda/format/` |
| Shared support | `lib/` |
| Native tests | `test/`, `make test-input-baseline` |
| Open issue records | `vibe/Lib_Input_Issue_Ledger.md` |
| Fixed issue records | `vibe/Lib_Input_Issue_Ledger (fixed).md` |
