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

<a id="li-016"></a>

### LI-016 — Cross-process SQLite scope contention lacks an admission/retry contract

**Status:** OPEN — provider capability/diagnostic gap; no lost-update or SQLite
corruption claim. **Observed:** 2026-11-11, macOS native debug, Lambda peer gate
on interpreter/AUTO/JIT. **Lambda blocker:** yes for the additional simultaneous
peer-credential administration acceptance case; no for sequential administration
or independently owned peer repositories. D9.1.1v4 / D11.1.2 / S12.4.4v4 apply.

Two Lambda processes concurrently run `invite` and `revoke` against one encrypted
credential row. One reports E400 `io.sqlite: cannot open transaction`; on JIT one
reports `commit outcome is uncertain; reopen before retrying`. Evidence in the
Lambda checkout: `temp/sync-peer/final-process-{interp,auto,jit}.log`. These
aggregate failure logs preserve the exact commands, errors and failing per-step
filenames; per-step directories are reused by later process qualifications. The
retained reproduction is `python3 test/sync_peer/test_peer.py --contention`
(after building Lambda). The normal peer gate excludes this still-unqualified
additional case explicitly.

Source evidence: `lib/rdb_sqlite.c:sqlite_tx_wrap` configures synchronization and
uses deferred `BEGIN`, not `BEGIN IMMEDIATE`; no busy timeout/handler is configured
there. The Lambda wrapper collapses failed acquisition to E400; failed commit is
conservatively uncertain under the documented contract. Deferred contention is an
expected SQLite behavior, but an acquire-before-read writer admission mechanism
and retryable/busy diagnostics are not exposed to this host integration. A common
cause for all reported acquisition/commit failures is not yet established.

The credential patch prevents stale-file overwrites by reading/modifying one
transactional row; it does not establish successful concurrent writer admission.
Do not add SQL lock tricks, retry uncertain commits, weaken the failed gate or
modify sibling source from Lambda. The affected concurrency work is stopped;
owner investigation should establish supported writer admission and outcome
inspection before Lambda expands that qualification. No owner source changed.

<a id="li-015"></a>

### LI-015 — All-frame GIF decoding bypasses the process memory budget

**Status:** RESOLVED, 2026-11-11. **Lambda task blocker:** cleared for Studio
PNG capture. Ownership **D9.1.1v4 / D9.1.2v2 / D9.1.3v2**. Sibling updates
were explicitly authorized by the user.

`image_gif_load_from_memory` checked each container against its decoded-byte
limit but allocated all composited frames without process admission. Several
individually valid animations on `https://studio.design/` retained
1,059,694,508 image-category bytes, preventing a viewport PNG allocation.
An allocation stack identifies `image_gif_load_from_memory` during intrinsic
image measurement. The existing preflight peak estimate is now returned and
admitted through `memtrack_allow_allocation` before giflib expansion. Structured
later-frame decoding reports `IMAGE_LIMIT_EXCEEDED` when admission fails.
The process limit is unchanged; independent first-frame decoding remains
available when the complete animation cannot fit.

Full image I/O passes 35/35, including palette/disposal/loop behavior, budget
rejection and still decoding; native Radiant GIF playback passes 23/23.
Studio now exports with 711,756,076 image-category bytes. Its selected initial
frame is mostly blank, so this is load/export qualification, not visual parity.
Evidence: Lambda `temp/online-recovery/studio-image-trace3-output.log`,
`studio-budget-fixed-output.log`, and canonical Studio capture; owner
`temp/online-gif-tests.log`; Radiant `temp/online-gif-player-tests.log`.

Coordinated Lambda **LR03-46** also reuses `map_put_heap` for private JS fields.
The input builder now grows storage before publishing its new descriptor,
preserving **D6.3** at its collecting growth callback. The callback sees the
complete old field list until the added lane and value exist. Lambda's forced-GC private growth regression passes; its full core gate passes
7,415/7,415 including 859 JS, 368 GC stress and 47 representation cases.
Qualification is recorded under Lambda LR03-46 and its final test logs.

<a id="li-014"></a>

### LI-014 — Online document captures hit TLS/connect failures despite intermittent curl success

**Status:** host-trust initialization repaired, 2026-11-11; residual public
origin/network failures remain open. **Lambda task blocker:** cleared for
verified Lua, Lua manual and Xiph captures; PIL and other transport failures
remain explicit after repeated live attempts. Acquisition ownership **D9.1.2v2**, project boundaries
**D9.1.1v4 / D9.1.3v2**. The user authorized sibling source updates.

The host trust adapter previously selected/loaded platform roots inside the
mbedTLS handshake callback, after TCP connection. Local first-record captures
show about 91–94ms cold delay after connect versus about 2ms with roots already
loaded. Cold Xiph probes failed 3/3; warmed probes succeeded 2/3, with an EOF
also observed after warming. This supports a host-side initialization defect
but does not explain every public request failure.

`curl_use_host_trust_store` now resolves roots before transport and passes the
captured chain through `CURLOPT_SSL_CTX_DATA`. The callback applies that chain;
CA and hostname verification remain enabled. The deterministic regression
`InputOwnedCurl.CapturesHostRootsBeforeStartingTransport` selects valid roots,
changes the ambient override before perform, and proves selection occurred
at configuration. It fails before the change (CURLcode60) and passes afterward.
All three InputOwnedCurl cases pass, including rejection of an incorrect CA
and hostname. No vendored libcurl/mbedTLS source was changed.

Input HTTP diagnostics now use the backend error buffer and distinguish
`CURLE_SSL_CONNECT_ERROR` from certificate-validation errors; handshake EOF
is no longer incorrectly described as a certificate error. Same-host live
Lua and Xiph renders succeeded with valid PNGs after the repair. PIL still
hit the existing 60s downloader timeout in that check and remains a failure;
no fallback/error page PNG is accepted as a loaded document. URLs, protocol
policy, deadlines and TLS validation are unchanged.

Evidence: owner `temp/li014-before.log`, `temp/li014-after.log`; Lambda
`temp/online-recovery/trust-clienthello.json`, curl cold/warm probes, and
`temp/online-recovery/li014-live/results.json` with per-page logs/PNGs.

<a id="li-013"></a>

### LI-013 — Gzip tar fixture is rejected by the native archive reader

**Status:** OPEN — cause unconfirmed. **Observed:** 2026-11-11, macOS,
`make test-input-baseline` (debug), including a rerun with local socket access.
**Lambda blocker:** no known block to the independent TeX script port. Ownership:
**D9.1.1v4 / D9.1.3v2**.

Input base `c5ca83e6d4d9793223117398cea5caea868d1040` plus the two-line HTTP POST
repair fails `ZipTest.NativeTarVariantsCaptureExactBytesAndNames` at
`test/test_zip_gtest.cpp:602`: opening `test/input/archive/sample.tar.gz` returns
null with `tar: invalid or unsupported archive`. The uncompressed tar fixture
succeeds; the gzip variant is expected to preserve the same entries and bytes.
The archive suite reports 34/35 passing, and a focused rerun reproduces the
failure in 2 ms. A separate baseline and rerun with local socket access report
the same failure, with the other 34 cases passing. No archive source, fixture,
expectation, or vendor dependency was edited for these observations.

Reproduce from input:

```sh
./build/test/test_zip_gtest --gtest_filter=ZipTest.NativeTarVariantsCaptureExactBytesAndNames
```

Evidence: `../../lambda/temp/kianhup-hang/input-baseline.log` and
`tar-control.log`. Existing LI-001 / LI-002 also fail the input aggregate;
all seven local HTTP cases and the Lambda viewer gate pass. No archive repair
or confirmed regression attribution is claimed.

<a id="li-012"></a>

### LI-012 — Initial report of the `sample.tar.gz` archive failure

**Status:** CONSOLIDATED into LI-013; the original ID is retained for references.
The repeated baseline and focused reproduction, cause status, blocker assessment,
and evidence are recorded under LI-013.

<a id="li-011"></a>

### LI-011 — Input dependency build pipeline and cross-platform qualification

**Status:** PARTIAL — macOS implementation verified; Linux and Windows need
verification. **Observed:** 2026-11-10, macOS, Lambda `make build`.
**Lambda blocker:** no on the verified macOS configuration. Authority:
**D9.1.1v4 / D9.1.3v2** (project ownership), **D9.1.5** (build pipeline).

Previously, input invoked CMake for utf8proc and Brotli, Meson/Ninja for
ThorVG, and curl's upstream Autotools `configure`. Meson's backend detection
produced the reported Ninja message. The owner now keeps the pre-split
Lambda recipes in `build_vendor_deps_config.json`, with a shared JSON-driven
Premake/Make builder for vendor and native archives. Curl uses compiler probes
and the upstream source manifest and templates, without running configure.
Pinned versions and the mbedTLS/HTTP2 profile remain in place.

On macOS, `make vendor-deps curl-deps native-deps JOBS=8` passed with CMake,
Meson, Ninja and Autotools programs blocked; `make test-build-dependencies`
passed ten tests. The input-owned curl tests passed, including the correct CA, a wrong CA and
a wrong hostname. The ThorVG manifest selects PNG and JPEG loaders on
macOS, Linux and Windows; actual Linux and Windows builds are not verified.
See `scripts/dependency_builder.py`, `scripts/build_vendor.py`,
`scripts/build_curl.py`, and `scripts/generate_native_deps_premake.py`.

<a id="li-007"></a>

### LI-007 — Shared header retains declarations from the second runtime retirement

**Status:** OPEN — owner header cleanup. **Observed:** 2026-11-10, Lambda
`5f3550a38` plus the second runtime audit. **Lambda blocker:** no; the
36 declarations have no source, generated-import or host-export consumer.
Authority: **D9.1.1v4 / D9.1.3v2** (header ownership), **D9.4.6** (host exports).

Lambda's second audit retires 88 functions and 43 runtime-only JIT import rows,
including old array fills/read adapters, scalar/boolean wrappers, map type-list
adapters and unused COW profiling adapters. Input's `lambda/lambda.h` still
advertises 36 of these removed functions. The expected result is that the
shared ABI header declares only the remaining implementation surface.

Exact declaration inventory and prepared, unapplied patch:
`../lambda/temp/mvp_legacy_reaudit/input-declarations.json` and
`../lambda/temp/mvp_legacy_reaudit/input-header.patch`. Reproduce the inventory
from Lambda with `python3 temp/mvp_legacy_reaudit/post_audit.py`; the original
caller audit is `selected-before.json` in the same directory. This is a stale
header issue, not a reproduced input parser defect. Lambda's runtime builds
without those definitions because no consumer remains. The native math catalog,
Radiant's `fn_call3_into` assembly alias and unary imported-predicate trampoline
are retained. No input/Radiant source or header is edited in this follow-up;
the earlier LI-006 exception was scoped to its region patch only.

Implementation and qualification record:
`../lambda/vibe/impl/Lambda_Impl_MIR_Full_Retirement.md`, second runtime audit.

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

<a id="li-012"></a>

### LI-012 — Empty POST reads terminal stdin and blocks page initialization

**Status:** RESOLVED, 2026-11-11.
**Lambda blocker:** removed; the plain terminal launch now renders the login form.
Owner authority: **D9.1.2v2**, **D9.1.1v4 / D9.1.3v2**. The user explicitly
authorized the owner-side fix on 2026-11-11 after the original boundary report.

Confirmed on input `c5ca83e6d4d9793223117398cea5caea868d1040` and Lambda
`456ba137dfe84a19eb59ef8c48f602fac7ffce28` plus its pending cookie-capability
repair. Plain interactive startup hangs before first layout. The public page's
Acro script performs synchronous template POST requests with no body.
`http_fetch2()` sets `CURLOPT_POST=1`, but `http_set_request_body()` returns
without setting a body or length when `FetchConfig.body` is null. Curl then
uses its default stdin reader. The main-thread sample shows
`js_xhr_send -> http_fetch2 -> curl_easy_perform -> Curl_client_read -> fread
-> __read_nocancel`. The process remains idle, waiting for terminal input;
the network timeout and CPU watchdog do not interrupt that blocking read.

Earlier automated runs had EOF on stdin and completed, hiding this separate
failure after Lambda's missing `navigator.cookieEnabled` was fixed. The
[curl POSTFIELDS contract](https://curl.se/libcurl/c/CURLOPT_POSTFIELDS.html)
requires an explicit empty buffer or zero POST length for an empty request.

Evidence: `../../lambda/temp/kianhup-hang/user-hang.log` and
`user-process.sample.txt`, captured from the user's process PID 53623 without
changing or terminating it. Reproduction: run the exact `view` command in a
terminal with stdin open. A regression must keep stdin open rather than rely
on subprocess EOF. Repair: set POST's initial length to zero, then let the
existing optional-body helper supply an explicit payload and length. No
vendor curl patch or Lambda-side workaround is needed.

**Repair and verification:** `lambda/input/input_http.cpp` `http_fetch2()`
now sets POST's initial length to zero before `http_set_request_body()`
applies any explicit payload. `test/test_http_gtest.cpp`
`EmptyPostDoesNotReadStdin` covers null and empty bodies with a pipe whose
writer remains open, and is selected by the owner offline baseline filter.
All seven local HTTP tests pass. Lambda's rebuilt debug host reaches first
render with live PTY stdin and the exact viewer argv, then a windowed PTY
replay confirms the complete form (`plain-pty.out`, `plain-pty.log`,
`login-window.png`, `login-window.log`). The viewer integration baseline
passes: 8,161 passed, 299 partial, 20 skipped of 8,480, zero failures
(`radiant-baseline.log`). The broader input baseline still fails LI-001,
LI-002 and the separately recorded LI-013; its HTTP cases all pass. Evidence
and the completion checkpoint are under `../../lambda/temp/kianhup-hang/`.
No credentials were entered or login submitted.


<a id="li-010"></a>

### LI-010 — TeX expl3 format startup exceeds Lambda's five-second debug test target

**Status:** RESOLVED — denied-format recursion stopped and TeX macro expansion
tuned. **Observed:** 2026-11-10,
Lambda HEAD `5c0d7c68c5f5dde248d6d951ec85b6d62f928569`, macOS arm64, debug.
**Lambda blocker:** no; both `AmbientExplThreeFormatIsNotAnAuthorityCache`
cases now meet the five-second target in the measurements below. Authority: **D9.1.1v4 /
D9.1.3v2** (ownership), **D10.2.2** (TeX expansion), **D11.2.1** (authority).

After rebuilding Lambda against the input archive, reproduce from `../lambda` with
`./test/test_security_runtime_gtest.exe --gtest_filter='InterpreterAndJit/SecurityRuntimeTest.AmbientExplThreeFormatIsNotAnAuthorityCache/*'`.
Before the fix, both cases passed but took 13.883 s and 8.482 s consecutively
in one process.
The user's target is less than five seconds per case without weakening the
trusted-format control or the scoped authority denial.

A three-second CPU sample of case `/0` places all 1,559 main-thread samples in
`tex::engine_create -> load_expl3_format -> build_expl3_image -> run_preload`,
including macro expansion, token scanning and temporary token-array allocation.
The test body calls the input-owned engine API directly; neither Lambda tier
execution nor MIR codegen lies on this sampled path. Relevant owner symbols:
`lambda/input/input-tex-main.cpp:927` (`engine_create`) and
`lambda/input/input-tex-format.cpp:535,559` (`build_expl3_image`,
`load_expl3_format`). The original warm `/1` cause was not sampled; expensive
image load/kernel setup was an unconfirmed candidate. A scoped engine must
build privately instead of trusting the ambient format.

Evidence: `../lambda/temp/test-runtime-tuning/security-before.log`,
`security-before.json`, `security-sample.txt`, and `security-profile.log`.

**Resolution:** When a requested expl3 format could not be built or loaded, the
kernel entry-point stubs retried `\RequirePackage{expl3}` until the
20-million-expansion budget expired. The engine now stops at that request
with `tex-expl3-unavailable`, after the scoped client has denied access.
Macro calls also avoid a temporary output list: parameterless bodies are
read from their arena-backed definitions, and substituted bodies are written
directly to the existing pooled input buffer. Expansion limits and private
format builds remain in force.

**Verification:** The Lambda security test was rebuilt from current source
against the modified input archive in this project's `temp/`. Three consecutive
processes passed both cases in 4.036–4.304 s cold and 4 ms warm per process
(`temp/li010-after.log`). The input TeX regression tests cover denied file
classification and reads, authorized private loading, first-request restart,
macro substitution, redefinition, and expansion budgets. All 151 recorded TeX
conformance lines match, and the expl3 package fixture expands identically with
ambient and authorized scoped resource access. The native fixture runners are
`temp/li010-conformance.cpp` and `temp/li010-expl3.cpp`.

<a id="li-009"></a>

### LI-009 — Shared-header migration blocks Lambda release tuning build

**Status:** RESOLVED — normal input and Lambda release builds passed after the owner header repair and Lambda consumer migration.
**Observed:** 2026-11-10, Darwin arm64, input HEAD `0b49aa017d` plus its
uncommitted shared-header migration. **Lambda blocker:** yes, candidate release
build for the Result55 regression-tuning task. Authority: **D9.1.1v4 /
D9.1.3v2** (source ownership), **D6.3** (shared runtime ABI).

From `../Lambda-opus`, reproduce with
`make build-lambda-split SPLIT_PROFILE=release SPLIT_OUTPUT=temp/result55_tuning/candidate1 JOBS=4`.
The input release build fails compiling `lib/datetime.c` with a conflicting
`DateTime` typedef (`lib/datetime.h:35` struct versus `lambda/lambda.h:592`
uint64_t), and compiling `lib/document_lifetime_guard.cpp` with unknown
`MemCategory` at `lib/string.h:42`. The latter include chain is
`mem.h -> memtrack.h -> lambda/lambda.h -> lib/datetime.h -> lib/string.h`,
which reaches the string API before the MemCategory declaration.

Complete compiler evidence:
`../Lambda-opus/temp/result55_tuning/build-candidate1.log` (lines 93–187).
Expected: the normal input release build succeeds with the shared headers
self-consistent in C and C++. These are observed include/type failures; the
intended final header layering is an owner decision. Header contents changed
again during diagnosis (datetime.h now forward-declares StrBuf), so the log
qualifies the failed build's source state, not a subsequent owner fix. No
input source or build rule was edited by the Lambda task. Lambda compiler
experiments remain unqualified until the normal release build succeeds.

**Verification, 2026-11-10:** the owner's subsequent header changes removed
the reported DateTime/MemCategory failures. A retry overlapped an active owner
curl build and failed with missing `conftest` files; process inspection confirmed
the concurrent configure/build. After that job finished, the normal split
release build reached Lambda and exposed stale `lib/lambda_api.h` and
`lib/lambda_alloca.h` consumers. Lambda-owned callers now use a shared
visibility-only wrapper over `lambda/lambda.h` and the existing `lib/alloca.h`.
The complete normal release build exited 0 and produced the unchanged-compiler
control at `../Lambda-opus/temp/result55_tuning/control`. Evidence:
`../Lambda-opus/temp/result55_tuning/build-control-migrated2.log`.
This task changed no input implementation/header/build source; owner edits
remain independently owned. Lambda tuning can resume.

<a id="li-006"></a>

### LI-006 — Region retirement needs the shared map allocator declaration updated

**Status:** RESOLVED — approved shared-header edit applied and verified on
2026-11-10. **Lambda blocker:** no. Authority: **D9.1.1v4 / D9.1.3v2**
(header ownership), **D6.3** (GC ownership).

The user explicitly approved the one-file exception for input-owned
`lambda/lambda.h`. The applied change exactly matches the reviewed patch at
`../lambda/temp/mvp_region_retirement/input-header.patch`: remove the region
opaque type and region-only declarations, and change
`map_alloc_for_type(TypeMap*, LambdaRegion*, int64_t)` to the GC-only
`map_alloc_for_type(TypeMap*, int64_t)`. No other input/Radiant source or header
was edited by Lambda for this retirement. The change remains uncommitted.

Lambda removed the allocator, block/cache helpers, heap cache fields, teardown
and region JIT entries, and migrated all 19 existing native calls plus MVP's
emitted call and import metadata. Input and Radiant have no implementation
callers. The former owner boundary was the only dependency blocking complete
retirement; no compatibility argument or alternate API was introduced.

Verification: input and Radiant archives and the private Lambda release rebuild
successfully. A tracked-source audit across all three projects finds no retired
region names. Nine selected native/profiling GTests, seven focused Python unit
methods, six JS map-growth/GC cases, four typed/untyped tree benchmark goldens
and four diagnostic root/memory parity cases pass. No owner or Lambda/Radiant
baseline suite was run. Receipts and source/binary identities:
`../lambda/temp/mvp_region_retirement/`; implementation record:
`../lambda/vibe/impl/Lambda_Impl_MIR_MVP_Regions_and_Profiling.md`.

The original issue was retained Full MIR declarations in the owner header,
not a reproduced input parser failure. The prior caller inventory/reproduction
is recorded in `../lambda/temp/mvp_region_retirement/source-audit.json`.

<a id="li-008"></a>

### LI-008 — ZIP output cannot store one entry uncompressed

**Status:** RESOLVED — per-file compression override implemented 2026-11-10.
**Observed:** 2026-11-10. **Lambda blocker:** no. Lambda's `.painting` files
are valid ZIPs without it.

ZIP output applied one `compression` option (`'deflate'` or `'stored'`) to every
entry. The method came from `write->method` in `lambda/input/input-zip.cpp` and
`options.method` in `lambda/io/zip_archive.cpp`. Container formats in the ODF,
OpenRaster and Krita `.kra` style require their first entry, `mimetype`, to be
stored uncompressed so file-type sniffers can read it at a fixed offset.
Lambda's painting files (`lambda.paint`, `Lambda_Pkg_Paint.md` PT12) follow that
layout but could only write `mimetype` first and deflated. Entry order is kept
when `deterministic` is false.

The original reproduction wrote
`<fs kind: 'dir', <fs name: "mimetype", kind: 'file', "x">>` with
`output(tree, path, {format: 'zip'})`; reading it with `input(path, 'zip')`
reported `'deflate'`, with no per-file way to select `'stored'`.

The constructed-file `compression` attribute now selects `'stored'` or
`'deflate'` for that file, overriding the archive default (S10.1.4). Absent
attributes inherit the default, while captured archive-node compression remains
source metadata. See [ZIP output](../README.md#zip-output) for an example and
the ordering and ZIP64 constraints on fixed-offset payloads.

Verification: `ZipTest.ConstructedFilesOverrideArchiveCompression` reproduced
both ignored override directions before the fix. Afterward all 35 cases in
`build/test/test_zip_gtest` passed, including fixed-offset payload, invalid
values, archive defaults, captured nodes, and deterministic ZIP64 coverage.

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

<a id="li-013"></a>

### LI-013 — Native TeX text-case expansion exhausts its budget on reference controls

**Status:** OPEN — cause unconfirmed. **Observed:** 2026-11-11, macOS debug
native TeX parser. **Script TeX-port blocker:** no; the experimental script
case converter explicitly reports unsupported reference forms it cannot handle.

Running `parse("\\ExplSyntaxOn\\text_uppercase:n{\\ref{aB}/\\pageref{aB}/\\eqref{aB}}\\ExplSyntaxOff", {type: "tex"})`
returns empty text and a `tex-budget` diagnostic after 20,000,000 macro calls,
followed by missing-number and missing-brace diagnostics. Evidence is in
`temp/expl3-script/native-case-refs.out`; the same probe shows `\\label` and
`\\cite` in a different case-conversion argument completing without a budget
diagnostic. Which control or combination triggers the exhaustion, and its root
cause, have not been established. Investigate the native expansion path before
changing its behavior.

<a id="li-014"></a>

### LI-014 — Native counter value scanning crosses an implicit expression boundary

**Status:** OPEN — root cause confirmed. **Observed:** 2026-11-11, macOS debug
native TeX parser; independently compared with pdfLaTeX. **Script TeX-port
blocker:** no for implementing the correct bounded evaluation, but native
parity for this case is blocked until the native path is fixed.

For `\begin{document}\newcounter{n}\setcounter{n}{\numexpr1+2}\arabic{n}\end{document}`,
native `parse(..., {type:"tex"})` emits `0` for `\arabic{n}` and reports
`latex-counter-value`, even though the counter eventually holds 3. The
pdfLaTeX reference emits 3; see `temp/expl3-script/counter-numexpr-ref.log` and
`counter-numexpr-debug.out`. In `input-tex-latex.cpp`, `eval_counter_value`
appends one frozen `\relax`, calls `scan_int`, and then reads another expanded
token to check for trailing material. An implicitly terminated `\numexpr`
consumes the only sentinel; that trailing-token read expands the following
`\arabic{n}` before `counter_command` stores the newly computed value. The
counter-value scan must remain bounded to its argument, including nested
expressions, without evaluating the following document tokens prematurely.
