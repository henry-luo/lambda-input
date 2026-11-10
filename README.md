# lambda-input

Shared data, native input/formatting, Mark APIs and support libraries for Lambda and Radiant (D9.1.1v3, D9.1.3v2, D9.1.4v2).

The owned source and archive manifest is `build_targets.json`; Lambda imports it for its integration builds. `make` builds the input and shared-library archives and their pinned native dependencies in this checkout. `PROFILE=debug` and `PROFILE=release` select separate first-party and codec archives. Set `CURL_PREFIX` for a static mbedTLS-backed curl, or use `make curl-deps` to build the pinned provider. Input does not import the Lambda runtime or Radiant (D9.1.1v4, D9.1.3v2).

The networking trust adapter requires libcurl built with the mbedTLS TLS backend; a system curl using Secure Transport or another backend is not compatible. The private update service also requires libcurl 8.10.1 or newer and asynchronous DNS.

`make test-input-baseline` builds and runs the input-owned library, parser, formatter, model, CSS, archive, network, Markdown, WPT HTML and YAML suites against the input archives. It prepares the pinned YAML test-suite checkout and uses `../lambda-test/markdown`; set `LAMBDA_TEST_DIR` to select a different shared corpus. GoogleTest, OpenSSL command-line tools and the external dependencies listed in the Makefile are required. The TLS and HTTP cases in this target use loopback sockets. `make test-network-online` separately runs the legacy tests that contact public HTTP endpoints. `make test-native` remains an equivalent entry point. Native test linking is currently configured for macOS.

The graph parser suite currently reports its existing `ParserLocBudget` failure: the checked-in Structurizr resolver and graph header exceed their recorded line limits. The other 19 graph cases pass; the budget check is retained. `TransitionTreeExternalParentTest.InadmissibleParentsAreDeclined` also reports an existing disagreement between the test and the external-parent implementation.

## ZIP output

Constructed `fs` file elements may set `compression: 'stored'` or
`compression: 'deflate'` to override the archive's `compression` option.
Files without this attribute inherit that option, which defaults to deflate.
The `compression_level` option applies to deflated files. Captured archive
nodes expose their original compression as metadata; the output option chooses
how to encode those nodes.

```lambda
pn save_example() {
    let tree = <fs kind: 'dir',
        <fs name: "mimetype", kind: 'file', compression: 'stored', "application/example">
        <fs name: "content.txt", kind: 'file', "document content">
    >
    output(tree, "temp/example.zip", {format: 'zip'})^
}
```

With the default `deterministic: false`, entries retain tree order, so the
example writes `mimetype` first. `deterministic: true` sorts entries by path;
forcing ZIP64 also adds a local-header extra field. Both choices matter to
formats that inspect the first member at a fixed byte offset.
