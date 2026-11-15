# Lib and Input Issue Ledger

Working issue record for `lambda-input` and its shared `lib` code. Input owns
these providers under D9.1.1v4 and D9.1.3v2. This ledger records defects,
test and build issues, and their evidence; it does not make design rulings.

Use stable IDs `LI-001`, `LI-002`, and so on. For each new issue record its
status, affected code or test, reproduction, observed and expected behavior,
and whether it blocks work in Lambda. Cite the applicable `S#` or `D#` ruling
when one covers the issue. Record uncertain causes as unconfirmed, link any
owner-side fix and verification, and retain the ID when the issue is resolved.


Resolved records are in the [fixed issue ledger](<Lib_Input_Issue_Ledger (fixed).md>).

## Open issues

<a id="li-063"></a>

### LI-063 — ZIP output cannot retain timestamps for modified members

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and Input `3e14a05`. **Lambda task blocker:** yes for a
metadata-preserving DOCX/EPUB editor. Input ownership is D9.1.1v4 and
D9.1.3v2; document and archive preservation is D9.3.4 / D9.5.5.

From the Lambda checkout run
`./test/test_lambda_gtest.exe --gtest_filter='*proc_editor_package_members*:*proc_editor_epub_chapter*'`.
The package editor reconstructs the changed XML member as a materialized
`<fs kind:'file'>`, preserving its `name`, bytes and `mode`, then writes the
archive through the ZIP writer. Python `zipfile` shows the original DOCX
`word/document.xml` timestamp `(2020, 1, 2, 3, 4, 6)` becomes
`(1980, 1, 1, 0, 0, 0)`; the original EPUB `Books/text/one.xhtml` timestamp
`(2026, 1, 1, 0, 0, 0)` also becomes `(1980, 1, 1, 0, 0, 0)`. Both retain
their `0600` permission bits. Evidence is in
`Lambda-opus/temp/edit-viewer/package-metadata.log`. Other members retain
their logical bytes; the archive is rebuilt and compressed representations and
unknown extra fields are outside the current ZIP writer contract.

`lambda/input/input-zip.cpp` `fs_output_walk` obtains timestamps only from
archive-backed `FsNode` objects. Constructed files have no supported way to
specify a modification timestamp, although their payloads are the supported
way to represent edits; rebuilt directory nodes likewise do not retain their
original directory metadata. Preserve and validate the available original
timestamp and directory mode when writing edited/rebuilt members. Keep the
user-facing package editor disabled until metadata and other package save
requirements have been addressed. No Input implementation was changed.


<a id="li-062"></a>

### LI-062 — Man parser includes .TH argument quotes in the title

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes for reliable
rich editing of man documents that use the Lambda man writer. Input ownership
is D9.1.1v4 and document processing is D9.3.4.

From the Lambda checkout run `./lambda.exe temp/edit-viewer/man-title-parse.ls`.
The input `.TH "TEST" "1"` yields an h1 whose text includes the double
quotes; `.TH TEST 1` yields `TEST` without them. The local `mandoc -Thtml`
independently renders both headers as the same `TEST(1)` title. Evidence:
`Lambda-opus/temp/edit-viewer/man-title-parse.log`,
`man-title-quoted.html`, and `man-title-unquoted.html` in the same directory.
The Lambda man writer emits quoted `.TH` arguments, so its output reparses
to a visibly different title. Writer metadata retention is a separate Lambda
editor concern; correcting the Input parser alone does not complete that work.

The exact Input parser cause is unconfirmed. LI-043 fixed the separate `\&`
zero-width escape issue. The editor retains source mode for man pages. No
Input implementation or reference was changed by this report.

<a id="li-061"></a>

### LI-061 — Textile span attributes are parsed as visible text

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes for safely
widening rich Textile admission. Input ownership is D9.1.1v4; document
processing is D9.3.4.

From the Lambda checkout run `./lambda.exe temp/edit-viewer/textile-style-roundtrip.ls`.
For `Text %{color:red}red% end.`, the parser produces unstyled nested spans
containing literal `{color:red}red`. Markdown conversion and formatting back
to Textile yield `%Text {color:red}red end.%`, losing the styled span and
changing the text. Textile's [span documentation](https://textile-lang.com/doc/spans)
uses the same inline CSS attribute syntax. Evidence is in
`Lambda-opus/temp/edit-viewer/textile-style-roundtrip.log`. The responsible
Input-side code and repair are unconfirmed.

After the LI-045 fix, the parser adds nested attribute-free spans for ordinary
Textile emphasis. Treating those wrappers as transparent could admit useful
documents, but would also hide the style loss above from the editor's semantic
round-trip check. Keep the existing source fallback for such documents until
the provider retains the styled span. No Input implementation or reference
was changed by this report.

<a id="li-059"></a>

### LI-059 — RST literal block round trip adds code-block newlines

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes for admitting
affected RST literal blocks to rich edit. Input owns the parser and formatter
under D9.1.1v4; D9.3.4 governs document processing.

From the Lambda checkout run `./lambda.exe temp/edit-viewer/rst-direct-roundtrip.ls`.
Parsing `Example::` followed by an indented `code` block yields code text
`code`. Formatting that parsed document as RST and parsing it again yields
code text `code\n\n`, and the original wrapper `div` is also removed. The
Markdown-mediated editor path yields `code\n\n\n`. Evidence is in
`Lambda-opus/temp/edit-viewer/rst-direct-roundtrip.log` and
`Lambda-opus/temp/edit-viewer/rst-current-probe.log`. Extra preformatted
newlines change document content; the editor therefore keeps the existing
source-mode fallback.

Resolved LI-047 repaired the literal introduction and code-block emission,
but this remaining round trip still changes code content. Whether the writer,
reader, or their contract owns the newlines is unconfirmed. No Input source
or reference was changed by this report.

<a id="li-058"></a>

### LI-058 — Calendar and contact parsers accept broken component boundaries

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes for safely
validating source-backed iCalendar and vCard property edits. Input ownership
is D9.1.1v4; document processing is D9.3.4 and parse-error acknowledgement
is S7.5.1v6.

From the Lambda checkout run `./lambda.exe temp/edit-viewer/contact-calendar-admission.ls`. An unclosed
`BEGIN:VCARD` and a card closed with `END:OTHER` both return a normal contact;
a property outside a vCard returns an empty contact list. For iCalendar an
unclosed `VEVENT`, a mismatched `END:VTODO`, an unclosed calendar, and a
property outside `VCALENDAR` all return success. In the first two cases the
VEVENT and its summary are silently absent from the result. A properly closed
calendar retains the event and acts as a control. The complete output is in
`Lambda-opus/temp/edit-viewer/contact-calendar-admission.log`.

The input parser in `lambda/input/input-vcf.cpp` accepts or finalizes incomplete
cards, while `lambda/input/input-ics.cpp` can leave an active component
unreported at a parent close or end of input; exact parser-state fixes are not
established. LI-040 fixed missing property-name/value separators, but did not
validate nesting or outer boundaries. Return parse errors for malformed
boundaries without silently dropping content. The editor retains source mode
until its provider can validate these documents. No Input implementation or
references are changed by this report.

<a id="li-057"></a>

### LI-057 — YAML parser accepts malformed scalars, aliases and collections

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes for safe YAML
property editing. Input ownership is D9.1.1v4, document processing D9.3.4,
and parse-error acknowledgement S7.5.1v6.

From the Lambda checkout run `./lambda.exe temp/edit-viewer/yaml-admission-probe.ls`.
The parser accepts an unknown double-quoted escape (`a: "bad\q"`), an
unterminated quoted scalar, an undefined alias (`a: *missing`), a tab used
for indentation, a doubled flow-sequence comma (`a: [1,,2]`), and duplicate
mapping keys. The independent local `js-yaml` parser rejects each input.
The Lambda probe output is `Lambda-opus/temp/edit-viewer/yaml-admission-probe.log`.
The precise Input-side causes are unconfirmed. Resolved LI-039 covers missing
flow delimiters, which the current parser does reject; it does not cover these
other malformed cases.

The Lambda editor leaves YAML in source mode because the current parser cannot
reliably validate a complete document or an edited value. No Input implementation
or references are changed by this report.

<a id="li-056"></a>

### LI-056 — TOML parser accepts invalid escapes, numbers and duplicate keys

**Status:** OPEN, reproduced 2026-11-15 against Lambda `e55450ec8` plus
editor work and input `3e14a05`. **Lambda task blocker:** yes, safe TOML
property editing needs the provider to reject malformed documents and edited
values. Input owns the parser under D9.1.1v4; D9.3.4 covers document processing
and S7.5.1v6 governs parse-error acknowledgement.

From the Lambda checkout, run `./lambda.exe temp/edit-viewer/toml-admission-probe.ls`.
The script-facing TOML parser accepts `name = "bad\q"`, `x = 01`, `x = 1__2`,
`x = 12_`, and repeated `x = 1` / `x = 2` assignments. Python's independent
`tomllib.loads` rejects all five; evidence is in
`Lambda-opus/temp/edit-viewer/toml-admission-probe.log`. The parser reports
unknown basic-string escapes as warnings and continues in
`lambda/input/input-toml.cpp`; the precise numeric and duplicate-key causes
are unconfirmed. The earlier LI-038 fix rejects trailing numeric content but
does not cover these spellings.

The editor currently leaves TOML in source mode. It cannot safely use the
provider to validate a property edit or a candidate document while invalid
values and duplicate assignments are admitted. This report makes no change
to Input implementation or references.

<a id="li-053"></a>

### LI-053 — Shared allocators lack ruled reference-counted lifetime

**Status:** OPEN — capability absent, 2026-11-14. **Lambda blocker:** not
established. D5.2.3 requires an atomic allocator reference count for shared
arenas and pools, with destruction at zero and exclusive reset/rewind.

The current `lib/mempool.h` and `lib/arena.h` expose no allocator retain or
release API, and `pool_destroy` and `arena_destroy` directly reclaim their
storage. `mem_context_destroy` also recursively releases registered child
nodes before destroying an allocator. The formal implementation inventory in
`Lambda_Formal_Design.md` likewise marks D5.2.3 reference counting as not
implemented. A retained allocator must keep its backing nodes, context and
any dependent cleanup state alive until its final release; adding a counter
to the allocator alone would leave those dependencies dangling. Reset and
rewind also need an exclusivity check.

This prevents using a retained source allocator as a general solution to the
source-owned type-contract lifetime in [LI-003](#li-003). It does not establish
that allocator retention is the only possible repair for that issue. No new
allocation mechanism or conservative GC scan is authorized by this record.

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

**Provider follow-up, 2026-11-14:** input now exposes `LambdaUvLoop` instances
with per-instance loop, prepare/check handles and drain context. Instances are
owned and driven by one thread; destruction returns `UV_EBUSY` while a user
handle or request remains or the loop is running. The legacy global API uses
the same checkpoint implementation and remains available for existing hosts.
Native tests exercise four concurrent owners, checkpoint ordering and
isolation, pending work, unreferenced handles, reentrant cleanup refusal and
legacy behavior (`temp/ledger-uv-instance-focused.log`); twenty repeated runs
pass (`temp/ledger-uv-instance-repeat.log`). This is an input
provider capability, not a repair of the reported Lambda hang: the scheduler,
JS event loop and other runtime consumers still call the legacy global API.
Lambda must migrate its context-owned consumers and qualify its concurrency
gate before LI-027 can close. D7.5.1 governs scheduler ownership.

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

2026-11-14 refresh: a bounded system-curl request to the same PIL URL failed
with `SSL_ERROR_SYSCALL` (exit 35), before an HTTP response. This is a system
transport control, not evidence of a new input-provider defect. See
`temp/ledger-network-pil.log`.

LI-016 is [archived in the fixed issue ledger](<Lib_Input_Issue_Ledger (fixed).md#li-016>).

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

2026-11-14 environment check: this host is Darwin arm64 and has no available
Docker, Podman, Lima, QEMU system emulator, or Wine command. Linux and Windows
native qualification remains pending; this check is not a platform build.

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

2026-11-14 review: the existing D3.4.3 implementation record in
`Lambda_Impl_DBench.md` explicitly admits `has_spread` when every stored
field is named, and rejects actual unnamed spread slots. The failing test
constructs an empty shape with only the marker, so it conflicts with that
documented implementation policy. A separate native control now checks both
the named-field admission and the unnamed-slot rejection. The original
assertion remains unchanged under this repository's test-preservation rule;
the aggregate failure remains open.

<a id="li-003"></a>

### LI-003 — Recheck DOM element backing-copy ownership

**Status:** PARTIAL — a source-storage lifetime defect is fixed; broader
non-view ownership remains unverified.
**Ownership checked:** 2026-11-09. **Lambda blocker:** not established.

The historical [RAD01-2](../../Lambda/vibe/radiant/Radiant_Issue_Ledger.md#rad01-2)
claimed that DOM element backing data was copied and could diverge. The current
input-owned `DomElement::create_in` in `lambda/input/css/dom_element.cpp` still
assigns the supplied `Element` into `element->elmt` and conditionally snapshots
backing data, but the old member names and constructor description are stale.
Review the current ownership and mutation invariants, including D5.5.1 and
D9.1.1v4, before deciding whether any defect remains.

2026-11-14 review: D5.5.3 requires a view occurrence to own its copied
document content. `build_dom_tree_with_parent` now uses `deep_copy_for_dom`
for ordinary view inputs, and the focused
`InputDomNativeTest.ViewOccurrencesOwnBackingAndLeaveSourceUnchanged` passes
for two occurrences and a mutated descendant. The historical `native_element`
member is gone; the registry separately records a render source, while DOM
reads and writes use the embedded backing. A second control verifies that
mapping and live mutation for a non-view wrapper. Non-view construction still
borrows shallow-copied data, and its behavior when the source has a shorter
lifetime than the document has not been qualified, so this issue remains open.
D5.5.2 permits a retained reference only when its GC root lives at least as
long as the document; the non-view control does not establish that host-side
lifetime condition.

**Source storage repair, 2026-11-14:** a non-view wrapper shallow-copies the
source element header. Adding an attribute through `MarkEditor` rebuilt the
wrapper's shape and freed its shared pool buffer, leaving the retained source
pointing at a freed block. If the shared type was a private clone, the same
edit could also change the source's shape. The editor now checks its document's
registered source before reusing a private shape or freeing old data; it
keeps the source-owned type and allocation intact. The test
`InputDomNativeTest.NonViewWrapperShapeChangesRetainSourceStorage` failed for
both shared and private shapes before the repair and passes afterward, along
with the other eight native DOM cases. Evidence: `temp/ledger-dom-regression-before.log`
and `temp/ledger-dom-suite-after.log`. Same-type writes through a directly
created non-view wrapper still share existing data. The shorter-source-lifetime
question remains unqualified; this partial repair does not close LI-003.

**Descendant reproduction:** a non-view tree's parent backing retains its
original child instead of the live DOM wrapper. Consequently
`dom_backed_node_index` returns -1 for the linked child, and a new child
attribute is absent when reading through the parent's Mark backing. Non-view
tree construction now uses the existing D5.5.2/D5.5.3 document-owned deep-copy
path, allocates each copied occurrence as DOM node storage, and retains the
source mapping. Focused tests cover two independent occurrences, descendant
edits and removal, and nested arrays without modifying the source. The native
regression `InputDomNativeTest.NonViewChildBackingTracksTheLiveWrapper` fails
before and passes after the repair. Evidence: `temp/ledger-dom-tree-before.log`
and `temp/ledger-dom-cow-qualified.log`. See the subsequent source-lifetime
control below for its scope; host-managed GC qualification remains open.

**Source-header lifetime repair, 2026-11-14:** even after a tree had been
copied into its document, a later shape edit dereferenced its retained source
header to discover whether its type and data were shared. A guard-page probe
reproduced the access after the source header became inaccessible
(`temp/ledger-dom-source-guard-before.log`). The node registry now records
shared storage while binding the valid source, and the editor consults that
record without rereading the source. Sixteen native DOM controls pass,
including view and non-view copied trees after a separate source pool is
destroyed, a guarded source header, and the existing shared-wrapper cases
(`temp/ledger-dom-source-focused.log`). These tests do not qualify foreign
storage borrowed by a directly created wrapper, render-map identity after
source retirement, or host-managed GC and compaction; LI-003 remains open.

**Retained field contract, 2026-11-14:** the copied element shape still
retains each source `ShapeEntry::type`. With a source-owned field contract,
protecting that contract's page after tree construction causes a subsequent
attribute read to fault (`temp/ledger-dom-contract-before.log`). The native
regression `InputDomNativeTest.CopiedTreeOwnsForeignFieldTypeContract` remains
failing for both view and non-view documents
(`temp/ledger-dom-contract-focused.log`). Shape contracts can contain nested
type graphs and nominal records whose pointer identity is significant under
S2.1.4/D3.1.1; copying only the base header or replacing the contract with a
generic type would not establish the required lifetime or preserve semantics.
The correct ownership of those contracts is still under investigation.
The cached `LaneStorageDesc::semantic_contract` and `base_contract` also
point into the source pool. The additional native nullable-contract control
exposes both the outer contract and its nested base; both controls fail in
view and non-view modes (`temp/ledger-dom-contract-graph-before.log`). The
document constructor receives an `Element*`, not its source `Input` or an
owner handle, and the shared allocator capability is open as [LI-053](#li-053).
