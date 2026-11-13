# Build the input archives directly from the same manifest consumed by Lambda.
PYTHON ?= python3
INPUT_DEPS_ROOT ?= .
PROFILE ?= debug
OWNED_CURL_PREFIX := $(abspath build/deps/curl)
CURL_PREFIX ?= $(OWNED_CURL_PREFIX)
PKG_CONFIG ?= pkg-config
PREMAKE5_BIN ?= premake5
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
PKG_CONFIG_PATH := $(abspath build/deps/utf8proc/lib/pkgconfig):$(abspath build/deps/brotli/lib/pkgconfig):$(abspath build/deps/thorvg/lib/pkgconfig):$(abspath $(CURL_PREFIX)/lib/pkgconfig)$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
TMPDIR := $(abspath temp)
export JOBS PKG_CONFIG_PATH PREMAKE5_BIN TMPDIR
INPUT_EXTRA_CPPFLAGS ?=
INPUT_CPPFLAGS = -Ibuild_temp/image-config/include -I. -Ilib/re2 -Ilib/font/woff2/include \
  -Ilib/image/libpng -Ilib/image/libjpeg-turbo/src -Ilib/image/libwebp/src \
  -Ilib/image/libavif/include -Ilib/image/libtiff/libtiff -Ilib/image/giflib \
  -Ilib/image/libnsbmp/include -I$(INPUT_DEPS_ROOT)/build_temp/image-config/include \
  -Ibuild/deps/utf8proc/include -I$(CURL_PREFIX)/include \
  -Ibuild/deps/thorvg/include -Ibuild/deps/thorvg/include/thorvg \
  -I/opt/homebrew/include -I/usr/local/include $(shell pkg-config --cflags libopenjp2 2>/dev/null) $(INPUT_EXTRA_CPPFLAGS)
ifeq ($(shell uname -s),Darwin)
# mbedtls@3 is keg-only and need not be linked into the Homebrew prefix.
INPUT_BREW_PREFIX ?= $(shell brew --prefix)
INPUT_CPPFLAGS += -I$(INPUT_BREW_PREFIX)/opt/mbedtls@3/include
PKG_CONFIG_PATH := $(INPUT_BREW_PREFIX)/opt/mbedtls@3/lib/pkgconfig:$(PKG_CONFIG_PATH)
PLATFORM_FLAGS ?= -mmacosx-version-min=15.7
else ifeq ($(shell uname -s),Linux)
# Input setup owns the Linux mbedTLS and mpdecimal providers.
INPUT_CPPFLAGS += -I$(INPUT_DEPS_ROOT)/build/deps/linux-support/include
PKG_CONFIG_PATH := $(abspath build/deps/linux-support/lib/pkgconfig):$(PKG_CONFIG_PATH)
endif
ifeq ($(PROFILE),debug)
PROFILE_FLAGS = -O3 -g -fno-omit-frame-pointer
PROFILE_DEFINES = -DDEBUG -DLAMBDA_JS_EXEC_PROFILE
else ifeq ($(PROFILE),debug_asan)
PROFILE_FLAGS = -Og -g -fno-omit-frame-pointer -fsanitize=address
PROFILE_DEFINES = -DDEBUG
else ifeq ($(PROFILE),release)
PROFILE_FLAGS = -O3
PROFILE_DEFINES = -DNDEBUG
else ifeq ($(PROFILE),release_profile)
PROFILE_FLAGS = -O3 -g -fno-omit-frame-pointer
PROFILE_DEFINES = -DNDEBUG -DLAMBDA_JS_EXEC_PROFILE
else ifeq ($(PROFILE),coverage)
PROFILE_FLAGS = -O0 -g -fprofile-instr-generate -fcoverage-mapping
PROFILE_DEFINES = -DDEBUG
else
$(error unsupported PROFILE $(PROFILE))
endif
CFLAGS ?= $(PROFILE_FLAGS)
CXXFLAGS ?= $(PROFILE_FLAGS) -Wno-return-type-c-linkage
LIB_SOURCES := $(shell $(PYTHON) scripts/build_sources.py lambda-lib)
DATA_SOURCES := $(shell $(PYTHON) scripts/build_sources.py lambda-data)
ifeq ($(strip $(LIB_SOURCES)),)
$(error lambda-lib source list is empty)
endif
ifeq ($(strip $(DATA_SOURCES)),)
$(error lambda-data source list is empty)
endif
LIB_OBJECTS := $(addprefix build/obj/lib/$(PROFILE)/,$(addsuffix .o,$(LIB_SOURCES)))
DATA_OBJECTS := $(addprefix build/obj/data/$(PROFILE)/,$(addsuffix .o,$(DATA_SOURCES)))
LIB_DEFINES = $(PROFILE_DEFINES) -DUTF8PROC_STATIC -DMEMPOOL_RUNTIME_VM_PROVIDER
DATA_DEFINES = $(PROFILE_DEFINES) -DSIMPLE_SCHEMA_PARSER -DPATH_NO_ITERATION -DLAMBDA_IO_STATIC_VALUES \
  -DUTF8PROC_STATIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION

.PHONY: all build-all-internal clean vendor-deps check-boundary check-curl check-native-test-deps configure curl-deps native-deps test-native test-input-baseline test-build-dependencies force-build-signature
# keep compiler output in one log and show only diagnostics plus the final status.
all:
	@$(PYTHON) scripts/quiet_build.py --label "lambda-input ($(PROFILE))" --log "temp/build-$(PROFILE).log" -- $(MAKE) --no-print-directory build-all-internal

build-all-internal: vendor-deps curl-deps native-deps build/lib/$(PROFILE)/liblambda-lib.a build/lib/$(PROFILE)/liblambda-input.a

# The input owner generates the pinned vendor configuration before compilation.
temp:
	@mkdir -p $@

configure: | temp
	$(PYTHON) scripts/generate_image_config.py build_native_deps_config.json

vendor-deps: configure
	$(PYTHON) scripts/build_vendor.py

# Build the pinned networking provider with the backend required by trust_store.
curl-deps: vendor-deps
	$(PYTHON) scripts/build_curl.py

# Build the owned codec, archive-reader and RE2 providers from the same manifest.
NATIVE_PROFILE ?= $(PROFILE)
NATIVE_FLAGS ?= $(PROFILE_FLAGS) $(PROFILE_DEFINES) -fPIC
native-deps: configure
	$(PYTHON) scripts/generate_native_deps_premake.py $(NATIVE_PROFILE)
	"$(PREMAKE5_BIN)" --file="$(abspath temp/native-deps/$(NATIVE_PROFILE)/premake5.lua)" gmake
	$(MAKE) -C "temp/native-deps/$(NATIVE_PROFILE)/make" config=$(NATIVE_PROFILE)_native -j$(JOBS) CC="$(CC)" CXX="$(CXX)" AR="$(AR)" CFLAGS= CXXFLAGS=

test-build-dependencies: | temp
	$(PYTHON) test/test_build_deps.py

export PROFILE CC CXX AR CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PLATFORM_FLAGS PKG_CONFIG INPUT_DEPS_ROOT INPUT_CPPFLAGS LIB_DEFINES DATA_DEFINES LIB_SOURCES DATA_SOURCES NATIVE_FLAGS NATIVE_PROFILE
force-build-signature:

build/firstparty-$(PROFILE)-build.json: force-build-signature scripts/update_build_signature.py
	$(PYTHON) scripts/update_build_signature.py

# The selected dependency headers and compiler flags are part of each object.
$(LIB_OBJECTS) $(DATA_OBJECTS): build/firstparty-$(PROFILE)-build.json scripts/generate_image_config.py scripts/build_config.py build_native_deps_config.json | configure check-curl vendor-deps

build/lib/$(PROFILE)/liblambda-lib.a: $(LIB_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

build/lib/$(PROFILE)/liblambda-input.a: $(DATA_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

build/obj/lib/$(PROFILE)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CFLAGS) -std=c17 -MMD -MP -MF $@.d $(if $(filter lib/procgen/%,$<),-ffp-contract=off) -c $< -o $@

build/obj/lib/$(PROFILE)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

build/obj/data/$(PROFILE)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(DATA_DEFINES) $(CFLAGS) -std=c17 -MMD -MP -MF $@.d -c $< -o $@

build/obj/data/$(PROFILE)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(DATA_DEFINES) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

# Reject an incompatible system curl before compiling any input objects.
check-curl: curl-deps
	@test -f "$(CURL_PREFIX)/include/curl/curl.h" && test -f "$(CURL_PREFIX)/lib/libcurl.a" && test -x "$(CURL_PREFIX)/bin/curl-config" || { echo "input build: run make curl-deps or set CURL_PREFIX to a static mbedTLS curl"; exit 1; }
	@test "$$("$(CURL_PREFIX)/bin/curl-config" --ssl-backends)" = mbedTLS || { echo "input build: curl must use only mbedTLS"; exit 1; }

check-boundary: all
	$(PYTHON) scripts/check_boundary.py

# Link all input providers so unused objects cannot conceal missing dependencies.
NATIVE_TEST_PACKAGES = gtest_main libutf8proc thorvg libuv libbrotlidec libmpdec libopenjp2 liblzma libzstd mbedtls mbedx509 mbedcrypto
NATIVE_TEST_SOURCES = test/test_input_blob_url_gtest.cpp test/test_input_dom_native_gtest.cpp \
  test/test_input_math_gtest.cpp test/test_input_test_helpers.cpp test/test_input_curl_gtest.cpp test/test_input_scalar_storage_gtest.cpp test/test_image_io_gtest.cpp
NATIVE_TEST_LIBS = build/lib/native/$(NATIVE_PROFILE)/libre2.a \
  build/lib/native/$(NATIVE_PROFILE)/liblambda-archive-readers.a \
  build/lib/native/$(NATIVE_PROFILE)/liblambda-image-codecs.a
NATIVE_TEST_EXTRA_LIBS ?=
check-native-test-deps: check-curl
	@test "$$(uname -s)" = Darwin || { echo "input native tests currently support macOS only"; exit 1; }
	@$(PKG_CONFIG) --exists $(NATIVE_TEST_PACKAGES) libcurl || { echo "input native tests: missing pkg-config dependencies"; exit 1; }

# Keep separate test executables for suites with their own global helpers.
define LINK_NATIVE_TEST
$(CXX) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" $(PKG_CONFIG) --cflags $(NATIVE_TEST_PACKAGES)) -std=c++17 $(3) $(1) \
  $(if $(4),$(4),-Wl,-force_load,build/lib/$(PROFILE)/liblambda-input.a -Wl,-force_load,build/lib/$(PROFILE)/liblambda-lib.a) \
  $(NATIVE_TEST_LIBS) $(CURL_PREFIX)/lib/libcurl.a $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" $(PKG_CONFIG) --libs $(NATIVE_TEST_PACKAGES)) \
  $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" $(PKG_CONFIG) --static --libs libcurl) -lbz2 -liconv \
  -framework CoreFoundation -framework Security -framework CoreServices -framework CoreText \
  -framework CoreGraphics -framework SystemConfiguration $(NATIVE_TEST_EXTRA_LIBS) -o $(2)
endef

# Owner-specific unit tests use the same input archives as downstream clients.
MIGRATED_LIB_TESTS = atomic base64 binsearch color datetime digest signature endian hash hex lru_cache \
  math_utils sort str str_scan strbuf strview stringbuf time_util uuid url url_extra arraylist \
  arena hashmap_helpers byte_storage checked_alloc file_module font_config font_context fts grep mail_imap mail_mime mem_context \
  mem_factory mem_vm mempool memtrack mime_detect scratch_arena shell span thread_pool woff2
MIGRATED_TEST_SOURCES = $(addprefix test/lib/test_,$(addsuffix _gtest.cpp,$(MIGRATED_LIB_TESTS))) \
  test/test_physics_gtest.cpp test/test_input_sourcepos_gtest.cpp test/test_tex_engine_gtest.cpp \
  test/css/test_css_tokenizer_gtest.cpp test/css/test_css_parser_gtest.cpp \
  test/css/test_css_engine_unit.cpp test/css/test_css_formatter_unit.cpp \
  test/css/test_css_parser_unit.cpp test/css/test_css_tokenizer_unit.cpp test/lib/test_avl_tree.cpp \
  test/css/test_css_style_node.cpp test/css/test_css_system.cpp test/css/test_css_engine_negative.cpp \
  test/css/test_css_roundtrip_unit.cpp test/css/test_compound_descendant_selectors.cpp \
  test/css/test_selector_groups.cpp test/css/test_css_integration_gtest.cpp \
  test/css/test_css_files_safe_gtest.cpp test/css/test_css_frameworks_gtest.cpp test/css/test_css_to_mark_gtest.cpp \
  test/test_mark_builder_gtest.cpp test/test_mark_builder_deepcopy_gtest.cpp test/test_mark_editor_gtest.cpp test/test_name_pool_gtest.cpp test/test_namespace_gtest.cpp test/test_path_gtest.cpp test/test_mark_reader_gtest.cpp test/test_entity_emoji_gtest.cpp test/test_input_rdb_gtest.cpp test/test_input_eps_gtest.cpp test/test_input_pdf_gtest.cpp test/test_html_gtest.cpp test/test_html_negative_gtest.cpp test/test_graph_formatter_gtest.cpp test/test_mdx_roundtrip_gtest.cpp test/test_jsx_roundtrip_gtest.cpp \
  test/test_error_tracking.cpp test/test_markup_roundtrip_gtest.cpp test/test_zip_gtest.cpp \
  test/test_radiant_agent_state_gtest.cpp test/test_network_scheduler_gtest.cpp test/parser/test_parser_robustness_gtest.cpp \
  test/test_navigation_gtest.cpp test/test_priority_queue_gtest.cpp test/test_recursion_guard_gtest.cpp test/test_security_policy_gtest.cpp \
  test/test_rdb_query_gtest.cpp test/markup/test_rst_functional_gtest.cpp \
  test/test_input_model_gtest.cpp test/test_input_roundtrip_gtest.cpp test/test_markup_modular_gtest.cpp \
  test/test_format_markup_gtest.cpp test/test_dir_gtest.cpp test/test_graph_mermaid_gtest.cpp \
  test/wpt/test_wpt_html_parser_gtest.cpp test/markup/test_markdown_gtest.cpp test/test_yaml_suite_gtest.cpp
MIGRATED_TEST_BINS = $(patsubst %.cpp,build/%,$(MIGRATED_TEST_SOURCES))

# Test utility objects are archived by their owner for native test clients.
TEST_HELPER_OBJECTS = build/obj/test-utils/$(PROFILE)/test_utils.o build/obj/test-utils/$(PROFILE)/test_utils_pool.o build/obj/test-utils/$(PROFILE)/test_input_test_helpers.o
build/obj/test-utils/$(PROFILE)/%.o: lib/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CFLAGS) -std=c17 -MMD -MP -MF $@.d -c $< -o $@

build/obj/test-utils/$(PROFILE)/test_input_test_helpers.o: test/test_input_test_helpers.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(DATA_DEFINES) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

build/lib/$(PROFILE)/liblambda-test-utils.a: $(TEST_HELPER_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(NATIVE_TEST_LIBS): | native-deps
$(CURL_PREFIX)/lib/libcurl.a: | curl-deps

build/test/%: test/%.cpp build/lib/$(PROFILE)/liblambda-test-utils.a build/lib/$(PROFILE)/liblambda-input.a build/lib/$(PROFILE)/liblambda-lib.a $(NATIVE_TEST_LIBS) $(CURL_PREFIX)/lib/libcurl.a | all check-native-test-deps
	@mkdir -p $(@D)
	$(call LINK_NATIVE_TEST,$<,$@,$(CXXFLAGS),build/lib/$(PROFILE)/liblambda-test-utils.a build/lib/$(PROFILE)/liblambda-input.a build/lib/$(PROFILE)/liblambda-lib.a)

# Exercise the runtime-profile scalar read without changing the input archive.
RUNTIME_SCALAR_TEST_OBJECT = build/obj/test-runtime/$(PROFILE)/lambda-data.cpp.o
RUNTIME_SCALAR_TEST_ARCHIVE = build/lib/$(PROFILE)/liblambda-input-runtime-scalar-test.a

$(RUNTIME_SCALAR_TEST_OBJECT): lambda/core/lambda-data.cpp build/firstparty-$(PROFILE)-build.json | configure check-curl vendor-deps
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(filter-out -DLAMBDA_IO_STATIC_VALUES,$(DATA_DEFINES)) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

$(RUNTIME_SCALAR_TEST_ARCHIVE): build/lib/$(PROFILE)/liblambda-input.a $(RUNTIME_SCALAR_TEST_OBJECT)
	@mkdir -p $(@D)
	cp $< $@
	$(AR) rcs $@ $(RUNTIME_SCALAR_TEST_OBJECT)

build/test/test_runtime_scalar_storage_gtest: test/test_runtime_scalar_storage_gtest.cpp $(RUNTIME_SCALAR_TEST_ARCHIVE) build/lib/$(PROFILE)/liblambda-lib.a $(NATIVE_TEST_LIBS) $(CURL_PREFIX)/lib/libcurl.a | all check-native-test-deps
	@mkdir -p $(@D)
	$(call LINK_NATIVE_TEST,$<,$@,$(CXXFLAGS),$(RUNTIME_SCALAR_TEST_ARCHIVE) build/lib/$(PROFILE)/liblambda-lib.a)

# YAML and Markdown corpora have independent upstream owners and pinned data.
.PHONY: setup-test-data test-network-online
setup-test-data: | temp
	$(PYTHON) scripts/setup_test_data.py

test-network-online: build/test/test_network_gtest build/test/test_http_gtest
	./build/test/test_network_gtest
	./build/test/test_http_gtest

.PHONY: test-perf test-api-headers
test-perf:
	@test "$(PROFILE)" = release || { echo "input test-perf requires PROFILE=release"; exit 1; }
	$(MAKE) PROFILE=release build/test/perf/test_avl_tree_perf
	./build/test/perf/test_avl_tree_perf

test-api-headers: | temp
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) -Ilib -std=c17 -fsyntax-only test/test_lambda_api_lib_only.c
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) -Ilib -std=c++17 -x c++ -fsyntax-only test/test_lambda_api_lib_only.c
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) -std=c17 -fsyntax-only test/test_lambda_api_headers.c
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) -std=c17 -DLAMBDA_TEST_DATETIME_FIRST -fsyntax-only test/test_lambda_api_headers.c
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CXXFLAGS) -std=c++17 -fsyntax-only test/test_lambda_api_headers.cpp

test-input-baseline: test-native

test-native: test-api-headers setup-test-data check-native-test-deps all native-deps $(MIGRATED_TEST_BINS) build/test/test_http_gtest build/test/test_runtime_scalar_storage_gtest
	@mkdir -p build/test
	$(call LINK_NATIVE_TEST,$(NATIVE_TEST_SOURCES),build/test/test_input_native)
	$(call LINK_NATIVE_TEST,test/test_graph_parser_gtest.cpp,build/test/test_graph_parser_native)
	$(call LINK_NATIVE_TEST,test/test_input_https_pull_gtest.cpp lib/https_pull.cpp,build/test/test_input_https_pull_native,-DLAMBDA_HTTPS_PULL_TESTING,build/lib/$(PROFILE)/liblambda-input.a build/lib/$(PROFILE)/liblambda-lib.a)
	@rc=0; $(PYTHON) test/run_native_tests.py ./build/test/test_input_native || rc=1; \
	  ./build/test/test_runtime_scalar_storage_gtest || rc=1; \
	  $(PYTHON) test/run_native_tests.py ./build/test/test_input_https_pull_native || rc=1; \
	  ./build/test/test_graph_parser_native || rc=1; \
	  ./build/test/test_http_gtest --gtest_filter=HttpInputTest.EmptyPostDoesNotReadStdin:HttpInputTest.SendsBrowserNavigationHeadersForDocumentDownloads:HttpInputTest.KeepsRedirectCookiesWithoutSessionJar:HttpInputTest.DecodesBrotliDocumentResponses:HttpInputTest.ErrorDocumentsRetainFinalResponseMetadataButResourcesReject:HttpInputTest.TruncatedErrorDocumentRemainsTransportFailure:HttpInputTest.TopLevelNavigationUsesPageLoadTimeout:HttpInputTest.DocumentCookiesKeepFinalPathAndHttpOnlyAttributes:HttpInputTest.CookiesShareAHostnameAcrossPorts:HttpInputTest.FileDocumentCookiesRetainTheirEmptyHostname || rc=1; \
	  for test_bin in $(MIGRATED_TEST_BINS); do ./$$test_bin || rc=1; done; exit $$rc

clean:
	rm -rf build

-include $(addsuffix .d,$(LIB_OBJECTS) $(DATA_OBJECTS) $(RUNTIME_SCALAR_TEST_OBJECT))
