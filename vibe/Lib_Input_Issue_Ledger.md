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

<a id="li-005"></a>

### LI-005 — Boundary check looks in the unprofiled object directory

**Status:** OPEN — build validation failure. **Observed:** 2026-11-10, macOS,
`make -j4 check-boundary` (debug). **Lambda blocker:** not established.
Authority: **D9.1.1v4 / D9.1.3v2** (input ownership boundary).

The input archives build, but `scripts/check_boundary.py` reports missing
dependency files because it checks `build/obj/<group>/<source>.o.d`, while the
Makefile writes them under `build/obj/<group>/<profile>/<source>.o.d`. The
expected result is for `make check-boundary` to inspect the selected profile's
dependency files and succeed when all dependencies stay inside input. An
in-memory audit using the debug-profile paths passed during LI-004 verification.

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

## Resolved issues

<a id="li-004"></a>

### LI-004 — Shared Lambda header retains retired Full MIR helper declarations

**Status:** RESOLVED — declarations retired on 2026-11-10. **Observed:** 2026-11-10.
**Lambda blocker:** no; Lambda has no remaining source/import/export consumer.
Authority: **D9.1.1v4 / D9.1.3v2** (header ownership), **D9.4.6** (host exports).

Lambda's follow-up to source retirement at `fd07a893f` removes 47 runtime helper
entry points. Its additional compiler/private-analysis cleanup stays Lambda-owned.
The caller audit covers current Lambda/MVP/T0/JS, tests, generated-name patterns,
assembly aliases, Jube export lists, and tracked input/Radiant sources. The
remaining references to removed helpers were 30 declarations in input-owned
`lambda/lambda.h`; Lambda did not edit that header. The input owner removed those
declarations after review.
No source consumer or host-export dependency was found. In particular,
`fn_call3_into` is **retained** because Radiant calls it through an assembly alias;
`fn_call_boxed_1_into` is retained for imported constrained-type predicates.

Reproduce the declaration inventory from the Lambda checkout with
`python3 temp/mvp_helper_retirement/helper_candidates.py` (audit input saved
before deletion), or read
`temp/mvp_helper_retirement/remaining-references.json` and `removed.json`.
Source proof and qualification status are tracked in
`vibe/impl/Lambda_Impl_MIR_Full_Retirement.md` in Lambda. This is coordinated API
cleanup from the retirement change, not a demonstrated input parser defect.

Verification: the debug input archives built successfully; tracked C, C++,
assembly and Python sources in input, Lambda and Radiant have no exact references
to the 30 names. `fn_call3_into` and the runtime unary boxed-call trampoline
remain. All 43 `MarkReaderTest` cases pass. The boundary dependency audit passes
when it reads the debug-profile object directory; the unmodified `make
check-boundary` target fails due to **LI-005**.

Historical owner declaration inventory (line numbers before removal):

- `Item list_fill` (line 1534)
- `void lambda_direct_field_store_image` (line 1550)
- `Item fn_call0_into` (line 1779)
- `Item array_float_literal_with_nulls` (line 2771)
- `Object* object_fill` (line 2833)
- `Item object_fill_checked` (line 2845)
- `uint8_t fn_string_char_eq_ascii` (line 2864)
- `uint8_t fn_string_char_eq` (line 2865)
- `Item fn_virtual_content_check` (line 2964)
- `Bool fn_not` (line 3046)
- `Item lambda_fn_colour_guard_args` (line 3073)
- `Bool lambda_type_value_is_exact` (line 3106)
- `Item lambda_fill_for_contract_int_lane` (line 3123)
- `Item lambda_array_empty_for_contract` (line 3139)
- `Item lambda_array_set_checked` (line 3157)
- `Item lambda_array_set_checked_inplace_item` (line 3167)
- `Item lambda_array_set_checked_lane` (line 3187)
- `Item lambda_array_set_checked_inplace_lane` (line 3190)
- `int64_t fn_len_a` (line 3323)
- `Item fn_join_consume_cert_array` (line 3469)
- `Item fn_join_consume_cert_array_item` (line 3470)
- `Item fn_join_consume_cert_array_pair` (line 3471)
- `Bool fn_starts_with_str` (line 3475)
- `Bool fn_ends_with_str` (line 3477)
- `Type* const_type_with_tl` (line 3541)
- `Item lambda_array_int_push_inferred_cow` (line 3753)
- `int64_t cow_path_set_packed_index` (line 3811)
- `Item cow_path_borrow_home_keys` (line 3819)
- `Item cow_place_leaf_fixed` (line 3830)
- `Item cow_place_leaf_keys` (line 3834)
