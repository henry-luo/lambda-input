# Lib and Input Issue Ledger

Working issue record for `lambda-input` and its shared `lib` code. Input owns
these providers under D9.1.1v4 and D9.1.3v2. This ledger records defects,
test and build issues, and their evidence; it does not make design rulings.

Use stable IDs `LI-001`, `LI-002`, and so on. For each new issue record its
status, affected code or test, reproduction, observed and expected behavior,
and whether it blocks work in Lambda. Cite the applicable `S#` or `D#` ruling
when one covers the issue. Record uncertain causes as unconfirmed, link any
owner-side fix and verification, and retain the ID when the issue is resolved.

Merge note (2026-11-13): parallel reports reused IDs already assigned in this
ledger, so the incoming records use unique IDs: Empty POST is LI-020; GIF
memory admission LI-021; online captures LI-022; Oklab/OkLCh colors LI-023;
cookie header terminators LI-024; cookie domains LI-025; Calmind CDN LI-026;
and the shared libuv loop LI-027. The tar follow-up remains consolidated
under LI-012. Existing evidence filenames keep their historical prefixes.

## Open issues

<a id="li-034"></a>

### LI-035 — Man parser renders the zero-width `\&` escape as text

**Status:** Open. **Blocks:** rich editing of man pages in `lambda edit`.

Reproduction from the Lambda checkout: run a Lambda script evaluating
`parse(".SH NAME\n\\&test\n", 'man')` and
`parse(".SH NAME\ntest\n", 'man')`. The first result contains a span with
text `&test`; the second contains text `test`. The roff `\&` escape is
zero-width, so these inputs should have the same visible paragraph text.
The checked output is in `Lambda-opus/temp/edit-viewer/man-input-probe.log`.
The parser's treatment of this escape is unconfirmed; this report identifies
observable behavior rather than an implementation cause. The Lambda man writer
uses `\&` to protect text, so its output cannot currently round-trip through
the input parser. The editor retains source mode until the input owner fixes it.

### LI-034 — CSV separator detection counts tabs inside quoted fields

**Status:** OPEN, reproduced 2026-11-13 on macOS with the Lambda-opus
`8b5ebe56e` debug snapshot plus editor work. **Lambda task blocker:** yes for
property editing CSV files whose first quoted record contains more tabs than
commas. Input ownership follows **D9.1.1v4**; **D9.3.4** covers document
processing.

`parse("\"a\t\tb\",c\n1,2\n", 'csv')` treats tabs inside the
quoted first field as separators and returns one field named `a\t\tb`
with value `1,2`, instead of the two CSV columns `a\t\tb` and `c`
with values `1` and `2`. The viewer uses this input parser. The editor's
matching delimiter choice cannot safely build the property tree for this
source and currently falls back to source editing.

Reproduce from the Lambda checkout with
`./lambda.exe temp/edit-viewer/csv-separator-probe.ls`. In
`lambda/input/input-csv.cpp`, `detect_csv_separator` counts every tab and
comma on the first physical line without tracking quoted fields. Detection
should ignore delimiters inside quoted fields, including escaped quotes and
multiline quoted records. No input implementation or reference was changed
by this report.

<a id="li-033"></a>

### LI-033 — Markdown footnote definitions lack source ranges

**Status:** OPEN, reproduced 2026-11-13 on macOS with the Lambda-opus
`8b5ebe56e` debug snapshot plus editor work. **Lambda task blocker:** yes for
showing and editing surrounding Markdown while retaining footnote definitions
as individually rendered view-only blocks. Input ownership follows **D9.1.1v4**;
source-backed document editing is covered by **D9.3.4**.

`parse(source, {type: 'markdown', sourcepos: true})` gives authored paragraph,
HTML and math blocks `sourcepos`, but moves footnote definitions into a generated
`<footnotes>` element whose individual `<footnote>` nodes have no original
source positions. The original source may contain multiple definitions and
unrelated link definitions in the same gap. Consequently the Lambda editor
cannot reliably associate each footnote with its exact authored lines, and
shows the combined gap as raw source instead of rendering the footnote. The
existing `edit_md_view_only` and `edit_md_view_only_keys` UI replays reproduce
this with the original editor as well as the current changes.

Reproduce from the Lambda checkout with
`./lambda.exe temp/edit-viewer/md-footnote-probe.ls` using
`test/ui/edit/kept.md`; its output shows the source-positioned body nodes,
the generated footnote without a `sourcepos`, and the combined source gap.
`lambda/input/markup/block/block_document.cpp` records source positions for
ordinary blocks but `parseFootnoteDefinition` constructs the note separately.
The input owner should retain each definition's original span when source
positions are requested, including multiline, duplicate and unused definitions.
No input implementation or reference was changed by this report.

<a id="li-032"></a>

### LI-032 — iCalendar and vCard parsers silently drop malformed content lines

**Status:** OPEN, reproduced 2026-11-13 on macOS with the Lambda-opus
`8b5ebe56e` debug snapshot plus editor work. **Lambda task blocker:** yes for
safe property editing of the affected format. Input ownership follows
**D9.1.1v4**; **S7.5.1v6** governs acknowledgement of a parse error.

Both `parse("BEGIN:VCALENDAR\nBROKEN\nEND:VCALENDAR\n", 'ics')` and `parse("BEGIN:VCARD\nBROKEN\nEND:VCARD\n", 'vcf')` succeed with empty entries, instead of reporting the line without a colon. The editor cannot rely on these results to validate a complete contact or calendar file. Relevant owner code is `lambda/input/input-ics.cpp` and `lambda/input/input-vcf.cpp`; the exact causes have not been confirmed. This is consistent with the older I-F10 observation in `vibe/impl/Lambda_Impl_Code_Review.md`.

Reproduce from the Lambda checkout with `./lambda.exe temp/edit-viewer/other-invalid.ls`.
A malformed document must produce an error rather than a successful partial
value. No input implementation or reference was changed by this report.

<a id="li-031"></a>

### LI-031 — YAML parser accepts an unterminated flow sequence

**Status:** OPEN, reproduced 2026-11-13 on macOS with the Lambda-opus
`8b5ebe56e` debug snapshot plus editor work. **Lambda task blocker:** yes for
safe property editing of the affected format. Input ownership follows
**D9.1.1v4**; **S7.5.1v6** governs acknowledgement of a parse error.

`parse("a: [1,\n", 'yaml')` returns a map containing `a: [1]` rather than an error for the unclosed sequence. The editor cannot rely on this parser to validate the complete YAML document. Relevant owner code is `lambda/input/input-yaml.cpp`; the exact cause has not been confirmed.

Reproduce from the Lambda checkout with `./lambda.exe temp/edit-viewer/other-invalid.ls`.
A malformed document must produce an error rather than a successful partial
value. No input implementation or reference was changed by this report.

<a id="li-030"></a>

### LI-030 — TOML parser accepts a numeric prefix followed by invalid text

**Status:** OPEN, reproduced 2026-11-13 on macOS with the Lambda-opus
`8b5ebe56e` debug snapshot plus editor work. **Lambda task blocker:** yes for
safe property editing of the affected format. Input ownership follows
**D9.1.1v4**; **S7.5.1v6** governs acknowledgement of a parse error.

`parse("x = 1junk\n", 'toml')` returns `{x: 1}` instead of rejecting the invalid numeric value. The editor cannot use that result to validate property edits or saved data. Relevant owner code is `lambda/input/input-toml.cpp`; the exact cause has not been confirmed.

Reproduce from the Lambda checkout with `./lambda.exe temp/edit-viewer/other-invalid.ls`.
A malformed document must produce an error rather than a successful partial
value. No input implementation or reference was changed by this report.

<a id="li-029"></a>

### LI-029 — JSON top-level parser accepts trailing non-whitespace

**Status:** OPEN, reproduced 2026-11-13 on macOS with Lambda-opus debug
`8b5ebe56e` plus the editor work. **Lambda task blocker:** yes for safely
admitting JSON property editing. Input ownership follows **D9.1.1v4**;
**S7.5.1v6** governs acknowledgement of an ordinary parse error.

From the Lambda checkout, run `./lambda.exe temp/edit-viewer/invalid.ls`
(the script evaluates `parse` on malformed JSON). In particular,
`parse("1abc", 'json')` and `parse("1 2", 'json')` both return `1`
instead of reporting the non-whitespace content after the top-level value;
`parse("", 'json')` returns null. Malformed JSON must report an error rather
than admit a prefix as the complete document. The editor cannot trust the
script-facing result when checking whether a JSON file or edited value is
safe to save.

In `lambda/input/input-json.cpp`, `parse_json` calls `parse_value` once and
returns without checking the remaining cursor; the existing native
`parse_json_to_item_strict` has a trailing-content check. The script-facing
`parse(..., 'json')` dispatch and the intended strictness policy should be
confirmed by the input owner. This report does not change input sources or
reference results.

<a id="li-028"></a>

### LI-028 — Unclosed optional math argument stalls native LaTeX parsing

**Status:** OPEN, reproduced 2026-11-13 with the local macOS Lambda snapshot.
**Lambda task blocker:** yes for malformed-input parity and safe full script
port qualification. D10.2.2 governs the current LaTeX input boundary.

Parsing `\begin{document}$\downarrow[unclosed$\end{document}` with
`parse(..., {type: "latex"})` did not return within five seconds; the timed
process was stopped. The fallback loop in `lambda/input/input-latex-c.cpp`
sees `[`, calls `parse_brack_group`, and repeats without advancing when the
bracket is unclosed: `consume_group_span` returns false for that form.
Parsing should terminate with a recoverable result or a diagnostic.

Reproduce with `python3 temp/expl3-script/verify-native-unclosed-math-bracket.py`;
the script and `native-unclosed-math-bracket-result.txt` are in the same
temporary directory. The behavior was discovered while qualifying the Lambda
Script TeX engine. Native input ownership is unchanged, and no input source
or reference result is modified by this report.

<a id="li-027"></a>

### LI-027 — Shared libuv loop spins during concurrent context scheduler teardown

**Status:** OPEN — process-global provider state confirmed; the exact intrusive-list
corruption/race is unconfirmed. **Observed:** 2026-11-12, macOS native debug,
Lambda checkout `85cc95ad4` plus the existing-ruling follow-up. **Lambda task
blocker:** yes for claiming the complete standalone shared-module concurrency
gate; no for the selected ledger repairs, whose Lambda and Test262 baselines pass.
Ownership: **D9.1.1v4 / D6.4.4 / D7.5.1** require a loop owned by each context.

Reproduction from `../Lambda-opus` after linking the current native host:
`./test/test_lambda_concurrency_gtest.exe --gtest_death_test_style=threadsafe`.
The preceding 97 native checks pass, and
`RuntimeGlobalsConcurrency.SharedChartAndPdfModulesStayStableAcrossEvalThreads`
passes its full 20-second concurrent phase. The next
`ModuleStateSlabsArePrivateToEachEvalContext` test spins during worker cleanup.
A one-second `sample` places its surviving worker in
`lambda_scheduler_destroy` (`lambda/runtime/concurrency.cpp:872`) ->
`lambda_uv_run_loop` (`lib/uv_loop.c:143`) -> `uv_run` -> `uv__run_prepare`.
The main thread waits in `pthread_join`; CPU is about one core and peak native
footprint is 3.5 GB. The unbounded run was stopped after capturing evidence.

`lib/uv_loop.c` still owns one process-global `g_loop`, `g_prepare`, `g_check`,
drain callback pair and active flags. Lambda's scheduler creates/closes each
context's wake handle on that shared loop. Four independent evaluator workers
can therefore enter its provider during teardown. A shared prepare/check-list
race is consistent with the sample, but an exact corruption-producing operation
has not been established. The existing Lambda Runtime Globals RG8 record
explicitly requires an instance-based `lambda_uv_*` API; moving only the loop
pointer or adding a Lambda-side serialization workaround does not implement it.

Evidence in `../Lambda-opus/temp/ledger-ruled-fixes/`:
`concurrency-complete.log`, `concurrency-hang.sample.txt`, and
`concurrency-sample.log`; implementation/checkpoint:
`../Lambda-opus/vibe/impl/Lambda_Impl_Ledger_Ruled_Followup (done).md`.
The test setup sources only add explicit acknowledgement of now-inferred chart
and PDF error outcomes; no workload, check, deadline or scheduler path is removed.
The affected additional qualification is stopped. No sibling implementation,
header, test or build source is changed by this report.

<a id="li-026"></a>

### LI-026 — Calmind CDN rejects a valid compressed resource request profile

**Status:** OPEN external interoperability limitation; native transport defect
unconfirmed, 2026-11-12. **Lambda blocker:** no; capture completes, but missing
first-party CSS and images leave a major visual difference. Network ownership
follows **D9.1.1v4 / D9.1.3v2**.

The current native capture of https://calmind.com/ receives HTTP403 HTML browser
checks for first-party CSS, scripts and images. Fresh headless Chromium receives
200 CSS for all three stylesheets. The resource manager already forwards the
owning document's Referer through CURLOPT_REFERER; adding the same Referer or
copying the current native User-Agent in system curl still receives 200 CSS.

Independent system-curl controls isolate an origin response variation:
`--compressed` advertises the available `deflate, gzip` codecs and receives
HTTP403, 5768 bytes of an HCDN browser-check page, with Vary:Accept-Encoding.
IPv4 gives the same result. Explicit gzip-only, identity, no encoding header or
Chromium's `gzip, deflate, br, zstd` header receives HTTP200 text/css. The valid
native compressed request is not proven malformed; no cookie, TLS, resource
scheduling or script-startup root cause is established by these controls.

Do not disable compression, spoof unsupported codecs, reorder headers for this
host or substitute its CSS solely to match the reference. Broader CDN/browser
challenge interoperability is outside this existing-bug milestone. Evidence:
Lambda `temp/online-visual-round13/calmind-corrected-controls.json`,
`calmind-encoding-controls.json`, saved response headers/bodies,
`calmind-native-probe.png` and `calmind-browser-current.json/png`. An early
User-Agent extraction concatenated following macros; those native/css-accept
controls are superseded by corrected-controls and are not evidence.

<a id="li-022"></a>

### LI-022 — Online document captures hit TLS/connect failures despite intermittent curl success

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

**Acquisition follow-up, 2026-11-12:** reran the remaining corpus using the
verified Lambda host. Exact PIL URL `https://www.lua.org/pil/contents.html`
loads in Chromium, and an input-owned curl probe also completed with HTTP200.
Three isolated Lambda captures still failed: two connection resets during
TLS handshake and one 60s timeout. System curl also timed out on that exact
URL in another attempt. The discrepancy remains intermittent; a new local
TLS defect or an origin/provider cause is **unconfirmed**. No error-page PNG
is accepted and no protocol, certificate or redirect limit was weakened.

**PIL follow-up, 2026-11-12:** The unchanged qualified Lambda host and the
official snapshot retry both loaded the page, so a valid 1200 × 800 capture is
now available. Three additional Lambda renders produced two TLS connection
resets and one HTTP 200; four alternating system-curl requests returned HTTP
200, while the standalone input-owned curl probe had one reset and three
HTTP 200 responses. A separate input-owned curl probe reproduced resets with
both default TLS and TLS 1.2, and also observed a connect timeout and an empty
reply. These observations locate failures in transport but do not establish
whether the remote peer, network path, or client TLS behavior causes them.
No reliable source-level repair is established. Evidence is in Lambda
`temp/pil-investigation/`; the original failed capture logs and manifest were
preserved there before the successful retry. Ownership remains **D9.1.2v2**.

Independent system-curl probes reproduce TCP connect timeouts for GNU and Qt,
connection refusal for Eigen, the Onfolk hostname mismatch (a Squarespace
certificate), and the expired ObservIQ certificate. The OS resolver cannot
produce addresses for `www.narrative.bi` or `empower.me`; local DNS queries
return NOERROR with no A answers. These are acquisition blockers in the
current environment, not evidence of a native rendering failure. Ownership
remains **D9.1.2v2**; attribution of the transport/DNS causes is not established.
Evidence: Lambda `temp/online-recovery-20261112/transport/`, `pil-render/`,
`pil-owned.log`, `pil-system.log`; per-case browser diagnostics live beside
`test/online/snapshots/capture_results.csv`. The capture helper now retains
selected origin headers, lifecycle events and failed/pending resources.

LI-016 is [resolved below](#li-016).

<a id="li-015"></a>

### LI-015 — Cross-process SQLite scope contention lacks an admission/retry contract

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
filenames; per-step directories are reused by later process qualifications. The retained reproduction
is `python3 test/sync_peer/test_peer.py --contention` (after building Lambda).
The normal peer gate excludes this still-unqualified additional case explicitly.

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


<a id="li-012"></a>

### LI-012 — Native tar variants test rejects sample.tar.gz

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

<a id="li-025"></a>

### LI-025 — Cookie domains include URL ports

**Status:** FIXED, 2026-11-12. **Lambda blocker:** cleared for cookie transport.
Ownership **D9.1.1v4 / D9.1.3v2**. Cookie host extraction reads Url.host, which
contains a nondefault port, instead of the canonical hostname. Cookies then
fail matching on another port of the same host, and curl rejects their imported
port-bearing domain for subresource requests. [RFC6265 §8.5](https://www.rfc-editor.org/info/rfc6265/#section-8.5)
does not isolate cookies by port.

Use url_get_hostname, retaining the established empty-authority behavior for
hostless file documents. CookiesShareAHostnameAcrossPorts fails before and
passes after; a distinct hostname still receives no cookie. The added
FileDocumentCookiesRetainTheirEmptyHostname and existing JS dom_document_cookie
golden protect file compatibility. Initial and script-created image requests
in the real HTTP export tests now carry the shared response/DOM cookies.
The three new input cases are included in the local baseline filter.

Final input aggregate:5224 passes,1 skip, with the same independent graph LOC
budget, transition-tree external-parent and sample.tar.gz failures (LI-001 /
LI-002 / LI-012). No affected source or fixture for those failures changed.
Full AST574 and real HTTP/session6 checks pass with native services available.
Evidence in Lambda temp/online-visual-round8/: http-before.log,
http-hostless.log, input-baseline-final.log, session-qualified.log,
js-baseline-ast-qualified.log. Capture qualification is in Lambda
 test/online/README.md; native RI-045 provides the missing export profile.

<a id="li-024"></a>

### LI-024 — Document response cookies retain HTTP line terminators

**Status:** FIXED, 2026-11-12. **Lambda blocker:** cleared for the reduced
export-session case. Ownership **D9.1.1v4 / D9.1.3v2**. The synchronous document
header callback copies the full curl span into cookie_jar_store. Unlike the
other network frontends, it leaves CRLF intact. A final Path=/ then contains
line endings and fails matching; a terminal HttpOnly/Secure name fails the
attribute comparison. The two-cookie local response exposes the private cookie
to document.cookie while omitting the ordinary visible cookie.
[libcurl HEADERFUNCTION](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html)
delivers bounded, non-NUL-terminated header lines including their terminator.

Use one cookie_jar_store_response_header helper to normalize the bounded span
and store the copied value. The document, synchronous-resource and curl-multi
callbacks reuse it, replacing duplicate normalization code. Allocation failure
aborts the transport. No vendored curl implementation is changed.
DocumentCookiesKeepFinalPathAndHttpOnlyAttributes fails before and passes after;
script-visible cookies exclude HttpOnly while request cookies include it.
All ten local HTTP cases and six real-renderer session tests pass. Evidence:
Lambda temp/online-visual-round8/{http-before.log,http-after.log,http-hostless.log,
session-qualified.log}. The full input gate retains the independent LI-001,
LI-002 and LI-012 failures; expectations and filters were not weakened.

<a id="li-023"></a>

### LI-023 — Missing Oklab/OkLCh components invalidate neutral theme colors

**Status:** FIXED, 2026-11-12. Input color computation ownership follows
**D9.1.1v4 / D9.1.2v2**. **Lambda blocker:** cleared for the reduced theme case.
Nuxt uses `oklch(14.5% 0 none)` in its neutral palette. The shared color helper
already retains a missing-channel bit and a zero used value, but the caller
explicitly disallowed missing components for perceptual functions. This
invalidated the palette fallback, leaving the dark page background transparent
and white headings invisible. CSS Color 4 §4.4 permits `none` in modern color
syntax and treats missing components as zero outside interpolation.

The caller now admits missing channels through the existing helper. Six
regression cases cover lightness/chroma/hue/alpha, stored missing bits,
serialization, round trips and RGB consumers. Two formerly-invalid assertions
were corrected to test actually invalid identifiers instead. The Nuxt hue
case fails before the change; all 112 CSS parser tests pass after. This does
not implement missing-component interpolation in color-mix or new color spaces.
Evidence: Lambda `temp/online-visual-round3/` reduced pages, `color-tests.log`
and Nuxt captures. The report must retain unrelated icon/mask limitations.
Spec: https://www.w3.org/TR/css-color-4/#missing

<a id="li-021"></a>

### LI-021 — All-frame GIF decoding bypasses the process memory budget

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

<a id="li-020"></a>

### LI-020 — Empty POST reads terminal stdin and blocks page initialization

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
LI-002 and the separately recorded LI-012; its HTTP cases all pass. Evidence
and the completion checkpoint are under `../../lambda/temp/kianhup-hang/`.
No credentials were entered or login submitted.

<a id="li-016"></a>

### LI-016 — Combined cloud/peer agent stalls during input/lib teardown

**Status:** RESOLVED — owner fix and combined-agent qualification complete (2026-11-12).
The user explicitly authorized `lambda-input` source changes for this issue.

**Confirmed cause:** InputManager releases retained Inputs in creation order.
Each Input's context was pushed onto a singly linked newest-first sibling list;
`detach_from_parent_locked` scanned that list to remove each oldest context.
The resulting quadratic pointer traversal dominated process exit. The stall was
not evidence of a graph cycle or skipped allocator destruction.

`MemContext` now retains a private previous-sibling link. Creation and removal
maintain both links under the existing registry lock, making arbitrary detach
constant time. Cleanup order and backing dependency teardown remain unchanged
(D5.2.1, D5.2.5). A mixed-order lifecycle test checks survivors, head/middle/tail
removal, reinsertion and final cascade destruction.

Release reproduction (`-O3 -DNDEBUG -std=c17`) compiles the exact before/after
source and exercises the same input-order topology. For 80,000 contexts, full
creation/destruction changed from about 3.03 seconds to a median 0.0097 seconds;
this is an isolated lifecycle measurement, not application-throughput evidence.
Owner focused suites pass **18 memory-context + 7 factory + 62 pool = 87 tests**.
Lambda's rebuilt native host passes **17/17 real-S3 cloud/combined-agent checks
on interpreter, AUTO and JIT**, with the original 180-second deadline unchanged.
The combined case is now mandatory in the default gate. Focused sync GC/poison
checks pass **9/9 per tier**; the ordinary sync sweep passes **51/51**.

Full input baseline: **5,220 passed, 3 failed, 1 skipped** across 106 executables
(5,224 tests). Only the already recorded LI-001 parser budget, LI-002 external-parent
expectation and LI-012 tar fixture fail; the font-dependent emoji test skips.
Full Lambda baseline: **7,493/7,494**, with only the existing TeX/expl3 golden
mismatch. These results do not mark those unrelated issues resolved.

Evidence: input `temp/li016/{before-source.json,after.json,mem_context.log,
mem_factory.log,mempool.log,input-baseline.log}`, Lambda
`temp/li016/{final-matrix.json,final-combined-*.log,final-stress-*.log,
lambda-baseline.log}` and the
[cloud implementation record](<../../lambda/vibe/impl/Lambda_Impl_Sync_Cloud (done).md>).
The former Lambda blocker is removed. LI-015 concurrent administration remains
separate and open.

<details><summary>Original observation and investigation — 2026-11-11</summary>

**Status:** OPEN — ownership cause unconfirmed; stack observation is in input/lib
cleanup, not proof of a provider defect. **Observed:** 2026-11-11, copied macOS
native host in Lambda's isolated `temp/sync-cloud/worktree`, interpreter and AUTO.
**Lambda blocker:** yes for the additional combined cloud-poll/peer-listener agent
acceptance case; ordinary one-shot cloud exchange/recovery passes independently.
D9.1.1v4, D6.3.3 and D11.1.2 apply. No sibling source was changed.

Run `test/sync_cloud/test_cloud.py` against the real local S3 test store. The final
combined case starts `cloud_cli.ls` action `run`, with four iterations and a
60-second cloud interval, and completes a peer transfer into that repository.
The peer client exits successfully and file bytes are installed, but the combined
agent does not finish within the existing 180-second test deadline. Both
interpreter and AUTO cases time out; no timeout was raised or harness patched.

The interpreter process reports completion (`executed=1`, about 172M nodes and
2334.50 MiB peak RSS) in its diagnostics before the driver timeout. A one-second
macOS stack sample finds the main thread in `lambda_main_finish` ->
`InputManager::destroy_global` -> `InputManager::reset_inputs` ->
`input_release_document_resources` -> `mem_context_destroy` ->
`destroy_ctx_locked` (`lib/mem_context.c:238`). Process inspection shows a CPU-bound
exit with a large memory footprint. Determine whether this is input/lib graph
teardown, an upstream Lambda ownership error, or pathological accumulated cleanup;
no cycle, allocator corruption or exact shared cause is confirmed.

Evidence relative to the Lambda checkout:
`temp/sync-cloud/worktree/temp/sync-cloud/combined-failure-{interp,auto}.log`,
`combined-evidence/{interp,auto}/31-run.err`, and
`agent-sample.txt`. The full process gate preserves this failure under
`--combined`; its ordinary cloud checks must not be represented as passing the
blocked combined gate. Combined-agent qualification work is stopped. Do not
change sibling sources, suppress teardown, extend timeouts, or add a process-exit
workaround from Lambda. Owner investigation is needed before advertising this
additional combined command as qualified.

</details>

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

<a id="li-017"></a>

### LI-017 — Native TeX leaks an environment end marker inside constructor arguments

**Status:** OPEN — cause confirmed for the observed path. **Observed:**
2026-11-12, macOS debug native TeX parser. **Script TeX-port blocker:** yes for
native-compatible handling of custom environments inside constructor arguments.

For `\newenvironment{foo}{B}{E}\begin{document}\textbf{A\begin{foo}X\end{foo}Z}\end{document}`,
native TeX output contains `\textbf{ABXE\.nvendZ}`. The reproduction and
native output are in `temp/expl3-script/constructor-env-probe.ls` and `.out`;
the staged Script result explicitly fails reconstruction for this case. In
`lambda/input/input-tex-latex.cpp`, `latex_arg_mode_env` pushes the end macro
and the internal environment-end primitive for a custom environment.
`arg_mode` in `lambda/input/input-tex-main.cpp` emits that unexpandable
primitive instead of executing it, leaking its internal control sequence into
the output and leaving the environment group unclosed. The internal name uses
`"\x02envend"`, whose C++ hexadecimal escape includes the `e` and produces
the observed leading dot. The native argument-mode handling and group lifetime
need correction before this case can be used as a parity reference.

<a id="li-018"></a>

### LI-018 — Native LaTeX environment scanning interprets raw inline verbatim

**Status:** OPEN — cause confirmed for the observed examples. **Observed:**
2026-11-12, macOS debug native LaTeX parser. **Script TeX-port blocker:** yes
for native AST comparison of affected valid verbatim content.

Native TeX preserves both `\begin{document}A\verb|x_%|B\end{document}` and
`\begin{document}A\verb|\end{document}|B\end{document}` without diagnostics,
but native LaTeX parsing reports an error for each; the same parser builds a
`verb` node for `\verb|xyz|`. Reproduction and output are
`temp/expl3-script/verb-native-digest-probe.ls` and `.out`. The document
`find_environment_end` scan in `lambda/input/input-latex-c.cpp` skips the
control word but does not skip its verbatim payload, so it treats `%` inside
that payload as a comment and `\end{document}` inside it as an environment
terminator. The scan needs to recognize raw inline spans before looking for
comments and environment boundaries.

<a id="li-019"></a>

### LI-019 — Native TeX discard boundaries become stale after cross-boundary unskip

**Status:** OPEN — root cause confirmed. **Observed:** 2026-11-12, macOS debug
native TeX parser. **Script TeX-port blocker:** yes for native-compatible
cross-boundary removal in discarded and nested box material.

For `\begin{document}A \insert0{\vadjust{\unskip}X}B\end{document}`, native
TeX emits `AXB`: the inner `\unskip` removes the preceding space, but `X`
leaks from the outer discarded insert. A similar case leaks an echoed global
macro definition. Reproduction and native output are
`temp/expl3-script/discard-unskip-detail.ls` and `.out`. The native log also
shows `arraylist_remove_oob` with a length of 4294967295; evidence is
`temp/expl3-script/discard-unskip-detail.log`.

`unskip` in `lambda/input/input-tex-main.cpp` removes the last output item
without adjusting open groups' `out_start` values. `finish_box` in
`lambda/input/input-tex-prims.cpp` subtracts those stale unsigned boundaries
from the shorter output length; it can either leave subsequent items outside
the intended discard slice or underflow. The array-list bounds check rejects
the invalid removal. Update the native boundary accounting and qualify nested
discarded groups before using these cases as port parity references.
