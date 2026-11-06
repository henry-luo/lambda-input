# lambda-input

Shared data, native input/formatting, Mark APIs and support libraries for Lambda and Radiant (D9.1.1v3, D9.1.3v2, D9.1.4v2).

This project is being extracted. Its canonical archive source manifest is `build_targets.json`; the Lambda build imports the same manifest. Native source files and tests live here, with migration links in both directions for the mixed directories. Those links mean this checkout is **not yet independently buildable**.

`make` builds the two input archives. External dependency headers and generated vendor configuration are supplied through `INPUT_DEPS_ROOT` and `INPUT_EXTRA_CPPFLAGS`. During migration the current macOS dependency setup can be selected with `make INPUT_DEPS_ROOT=../Lambda`. `make INPUT_DEPS_ROOT=../Lambda check-boundary` verifies that compiled first-party sources and headers do not escape through migration links. Lambda can link these archives with `make build-lambda-split`. Vendor dependency packaging, independently built tests, and removal of the remaining migration links remain required before distribution.

For the current migration, clone the companion repositories as siblings named `Lambda`, `lambda-input`, and `radiant`. The `Lambda` checkout supplies the temporary runtime and vendor build dependencies.
