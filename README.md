# lambda-input

Shared data, native input/formatting, Mark APIs and support libraries for Lambda and Radiant (D9.1.1v3, D9.1.3v2, D9.1.4v2).

This project is being extracted. Its canonical archive source manifest is `build_targets.json`; the Lambda build imports the same manifest. Native source files and tests live here, with migration links in both directions for the mixed directories. Those links mean this checkout is **not yet independently buildable**.

`make curl-deps` downloads and verifies the pinned curl source, then builds an mbedTLS-backed static curl under `build/deps/curl`. It requires mbedTLS and nghttp2 development packages discoverable with `pkg-config`. `make` generates the pinned codec configuration locally and builds the two input archives. External dependency headers are supplied by the system or through `INPUT_DEPS_ROOT` and `INPUT_EXTRA_CPPFLAGS`. During migration the current macOS dependency setup can be selected with `make INPUT_DEPS_ROOT=../Lambda`. `make INPUT_DEPS_ROOT=../Lambda check-boundary` verifies that compiled first-party sources and headers do not escape through migration links. Lambda can link these archives with `make build-lambda-split`. Input has no source-tree symlinks back to Lambda. Vendor dependency packaging and additional independent tests remain required before distribution; Lambda still has migration links to input-owned files.

The networking trust adapter requires libcurl built with the mbedTLS TLS backend; a system curl using Secure Transport or another backend is not compatible. The private update service also requires libcurl 8.10.1 or newer and asynchronous DNS.

For the current migration, clone the companion repositories as siblings named `Lambda`, `lambda-input`, and `radiant`. The `Lambda` checkout supplies the temporary runtime and vendor build dependencies.

On macOS, `make native-deps` builds the pinned RE2, archive-reader and image-codec archives in debug mode. After `make curl-deps`, `make test-native` builds and runs the input-owned native tests using those archives and the locally built curl. The tests require GoogleTest, OpenSSL command-line tools and the other external libraries discoverable through `pkg-config`; they exercise the network trust adapter and private HTTPS pull against a loopback server. Set `CURL_PREFIX` to select a different static mbedTLS curl installation. This test currently covers macOS debug builds only.

The graph parser suite currently reports its existing `ParserLocBudget` failure: the checked-in Structurizr resolver and graph header exceed their recorded line limits. The other 19 graph cases pass; the budget check is retained.
