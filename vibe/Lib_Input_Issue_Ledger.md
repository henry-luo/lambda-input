# Lib and Input Issue Ledger

Working issue record for `lambda-input` and its shared `lib` code. Input owns
these providers under D9.1.1v4 and D9.1.3v2. This ledger records defects,
test and build issues, and their evidence; it does not make design rulings.

Use stable IDs `LI-001`, `LI-002`, and so on. For each new issue record its
status, affected code or test, reproduction, observed and expected behavior,
and whether it blocks work in Lambda. Cite the applicable `S#` or `D#` ruling
when one covers the issue. Record uncertain causes as unconfirmed, link any
owner-side fix and verification, and retain the ID when the issue is resolved.

## Open issues

<a id="li-001"></a>

### LI-001 — Graph parser line-budget test fails

**Status:** OPEN — validation failure. **Observed:** 2026-11-09, macOS,
`make test-input-baseline` (debug). **Lambda blocker:** not established.

`GraphParserTest.ParserLocBudget` in `test/test_graph_parser_gtest.cpp`
reports 306 resolver lines against a limit of 273, 108 header lines against
107, and 4,443 combined lines against 4,409. The other 19 graph parser cases
passed in the same run. Reproduce with
`./build/test/test_graph_parser_native --gtest_filter=GraphParserTest.ParserLocBudget`
after building the baseline. The expected result is that the unchanged budget
check passes; a source simplification or other resolution requires investigation.
This observation does not establish a functional parser regression.

<a id="li-002"></a>

### LI-002 — External-parent admissibility test disagrees with implementation

**Status:** OPEN — expected behavior and root cause need investigation.
**Observed:** 2026-11-09, macOS, `make test-input-baseline` (debug).
**Lambda blocker:** not established. D3.4.3 requires a transition tree not
to write to an external parent; it does not by itself establish the disputed
`has_spread` admission rule.

`TransitionTreeExternalParentTest.InadmissibleParentsAreDeclined` in
`test/test_mark_builder_gtest.cpp` expects `type_tree_add_map_field_chars`
to return null for an external map marked `has_spread`, but it returns a
non-null child. `external_parent_admissible` in `lambda/input/input.cpp`
currently allows named stored fields for runtime-key literals that keep this
flag. Determine the intended admission rule and repair the implementation or
the test at the root cause; do not change the expectation merely to pass.
Reproduce with `./build/test/test_mark_builder_gtest --gtest_filter=TransitionTreeExternalParentTest.InadmissibleParentsAreDeclined`
after building the baseline.

<a id="li-003"></a>

### LI-003 — Recheck DOM element backing-copy ownership

**Status:** UNVERIFIED — historical issue requires owner review.
**Ownership checked:** 2026-11-09. **Lambda blocker:** not established.

The historical [RAD01-2](../../Lambda/vibe/radiant/Radiant_Issue_Ledger.md#rad01-2)
claimed that DOM element backing data was copied and could diverge. The current
input-owned `DomElement::create_in` in `lambda/input/css/dom_element.cpp` still
assigns the supplied `Element` into `element->elmt` and conditionally snapshots
backing data, but the old member names and constructor description are stale.
Review the current ownership and mutation invariants, including D5.5.1 and
D9.1.1v4, before deciding whether any defect remains. No behavioral regression
has been reproduced in this review.

<a id="li-004"></a>

### LI-004 — ZIP output cannot store one entry uncompressed

**Status:** OPEN — missing capability; confirmed by source reading.
**Observed:** 2026-11-10. **Lambda blocker:** no. Lambda's `.painting` files
are valid ZIPs without it.

ZIP output applies one `compression` option (`'deflate'` or `'stored'`) to every
entry. The method comes from `write->method` in `lambda/input/input-zip.cpp` and
`options.method` in `lambda/io/zip_archive.cpp`. Container formats in the ODF,
OpenRaster and Krita `.kra` style require their first entry, `mimetype`, to be
stored uncompressed so file-type sniffers can read it at a fixed offset.
Lambda's painting files (`lambda.paint`, `Lambda_Pkg_Paint.md` PT12) follow that
layout but can only write `mimetype` first and deflated. Entry order is kept
when `deterministic` is false.

Reproduce: write `<fs kind: 'dir', <fs name: "mimetype", kind: 'file', "x">>`
with `output(tree, path, {format: 'zip'})`, then read it back with
`input(path, 'zip')`; the member's `compression` attribute is `'deflate'`.
Expected: a per-file attribute on constructed trees, for example
`<fs name: "mimetype", kind: 'file', compression: 'stored', ...>`, that
overrides the archive default for that entry (S10.1.4 output options).
