# Build the input archives directly from the same manifest consumed by Lambda.
PYTHON ?= python3
INPUT_DEPS_ROOT ?= .
OWNED_CURL_PREFIX := $(abspath build/deps/curl)
CURL_PREFIX ?= $(if $(filter .,$(INPUT_DEPS_ROOT)),$(OWNED_CURL_PREFIX),$(INPUT_DEPS_ROOT)/mac-deps/curl-8.10.1)
PKG_CONFIG ?= pkg-config
INPUT_EXTRA_CPPFLAGS ?=
INPUT_CPPFLAGS = -Ibuild_temp/image-config/include -I. -Ilib/re2 -Ilib/font/woff2/include \
  -Ilib/image/libpng -Ilib/image/libjpeg-turbo/src -Ilib/image/libwebp/src \
  -Ilib/image/libavif/include -Ilib/image/libtiff/libtiff -Ilib/image/giflib \
  -Ilib/image/libnsbmp/include -I$(INPUT_DEPS_ROOT)/build_temp/image-config/include \
  -I$(INPUT_DEPS_ROOT)/build_temp/utf8proc -I$(CURL_PREFIX)/include \
  -I$(INPUT_DEPS_ROOT)/mac-deps/thorvg/inc -I$(INPUT_DEPS_ROOT)/mac-deps/thorvg/src/bindings/capi \
  -I/opt/homebrew/include -I/usr/local/include $(shell pkg-config --cflags libopenjp2 2>/dev/null) $(INPUT_EXTRA_CPPFLAGS)
ifeq ($(shell uname -s),Darwin)
PLATFORM_FLAGS ?= -mmacosx-version-min=15.7
endif
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g -Wno-return-type-c-linkage
LIB_SOURCES := $(shell $(PYTHON) scripts/build_sources.py lambda-lib)
DATA_SOURCES := $(shell $(PYTHON) scripts/build_sources.py lambda-data)
ifeq ($(strip $(LIB_SOURCES)),)
$(error lambda-lib source list is empty)
endif
ifeq ($(strip $(DATA_SOURCES)),)
$(error lambda-data source list is empty)
endif
LIB_OBJECTS := $(addprefix build/obj/lib/,$(addsuffix .o,$(LIB_SOURCES)))
DATA_OBJECTS := $(addprefix build/obj/data/,$(addsuffix .o,$(DATA_SOURCES)))
LIB_DEFINES = -DDEBUG -DLAMBDA_JS_EXEC_PROFILE -DUTF8PROC_STATIC -DMEMPOOL_RUNTIME_VM_PROVIDER
DATA_DEFINES = -DDEBUG -DLAMBDA_JS_EXEC_PROFILE -DSIMPLE_SCHEMA_PARSER -DPATH_NO_ITERATION -DLAMBDA_IO_STATIC_VALUES \
  -DUTF8PROC_STATIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION

.PHONY: all clean check-boundary check-curl check-native-test-deps configure curl-deps native-deps test-native force-build-signature
all: build/lib/liblambda-lib.a build/lib/liblambda-input.a

# The input owner generates the pinned vendor configuration before compilation.
configure:
	$(PYTHON) scripts/generate_image_config.py build_native_deps_config.json

# Build the pinned networking provider with the backend required by trust_store.
curl-deps:
	$(PYTHON) scripts/build_curl.py

# Build the owned codec, archive-reader and RE2 providers from the same manifest.
NATIVE_PROFILE ?= debug
NATIVE_FLAGS ?= -O3 -g -DDEBUG -DLAMBDA_JS_EXEC_PROFILE -fPIC -fno-omit-frame-pointer
native-deps: configure
	$(PYTHON) scripts/generate_native_deps_makefile.py $(NATIVE_PROFILE)
	$(PYTHON) scripts/update_build_signature.py native
	$(MAKE) -f build/native-deps.mk all PLATFORM_FLAGS="$(PLATFORM_FLAGS)"

export CC CXX AR CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PLATFORM_FLAGS PKG_CONFIG INPUT_DEPS_ROOT INPUT_CPPFLAGS LIB_DEFINES DATA_DEFINES LIB_SOURCES DATA_SOURCES NATIVE_FLAGS NATIVE_PROFILE
force-build-signature:

build/firstparty-build.json: force-build-signature scripts/update_build_signature.py
	$(PYTHON) scripts/update_build_signature.py

# The selected dependency headers and compiler flags are part of each object.
$(LIB_OBJECTS) $(DATA_OBJECTS): build/firstparty-build.json scripts/generate_image_config.py scripts/build_config.py build_native_deps_config.json | configure check-curl

build/lib/liblambda-lib.a: $(LIB_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

build/lib/liblambda-input.a: $(DATA_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

build/obj/lib/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CFLAGS) -std=c17 -MMD -MP -MF $@.d $(if $(filter lib/procgen/%,$<),-ffp-contract=off) -c $< -o $@

build/obj/lib/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(LIB_DEFINES) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

build/obj/data/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(DATA_DEFINES) $(CFLAGS) -std=c17 -MMD -MP -MF $@.d -c $< -o $@

build/obj/data/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(DATA_DEFINES) $(CXXFLAGS) -std=c++17 -MMD -MP -MF $@.d -c $< -o $@

# Reject an incompatible system curl before compiling any input objects.
check-curl:
	@test -f "$(CURL_PREFIX)/include/curl/curl.h" && test -f "$(CURL_PREFIX)/lib/libcurl.a" && test -x "$(CURL_PREFIX)/bin/curl-config" || { echo "input build: run make curl-deps or set CURL_PREFIX to a static mbedTLS curl"; exit 1; }
	@test "$$("$(CURL_PREFIX)/bin/curl-config" --ssl-backends)" = mbedTLS || { echo "input build: curl must use only mbedTLS"; exit 1; }

check-boundary: all
	$(PYTHON) scripts/check_boundary.py

# Link all input providers so unused objects cannot conceal missing dependencies.
NATIVE_TEST_PACKAGES = gtest_main libutf8proc thorvg libuv libbrotlidec libmpdec libopenjp2 liblzma libzstd openssl
NATIVE_TEST_SOURCES = test/test_input_blob_url_gtest.cpp test/test_input_dom_native_gtest.cpp \
  test/test_input_math_gtest.cpp test/test_input_test_helpers.cpp test/test_input_curl_gtest.cpp test/test_image_io_gtest.cpp
NATIVE_TEST_LIBS = build/lib/native/$(NATIVE_PROFILE)/libre2.a \
  build/lib/native/$(NATIVE_PROFILE)/liblambda-archive-readers.a \
  build/lib/native/$(NATIVE_PROFILE)/liblambda-image-codecs.a
NATIVE_TEST_EXTRA_LIBS ?=
check-native-test-deps: check-curl
	@test "$$(uname -s)" = Darwin || { echo "input native tests currently support macOS only"; exit 1; }
	@$(PKG_CONFIG) --exists $(NATIVE_TEST_PACKAGES) || { echo "input native tests: missing pkg-config dependencies"; exit 1; }

# Keep separate test executables for suites with their own global helpers.
define LINK_NATIVE_TEST
$(CXX) $(PLATFORM_FLAGS) $(INPUT_CPPFLAGS) $(shell $(PKG_CONFIG) --cflags $(NATIVE_TEST_PACKAGES)) -std=c++17 $(3) $(1) \
  $(if $(4),$(4),-Wl,-force_load,build/lib/liblambda-input.a -Wl,-force_load,build/lib/liblambda-lib.a) \
  $(NATIVE_TEST_LIBS) $(CURL_PREFIX)/lib/libcurl.a $(shell $(PKG_CONFIG) --libs $(NATIVE_TEST_PACKAGES)) \
  -lmbedtls -lmbedx509 -lmbedcrypto -lnghttp2 -lbrotlicommon -lz -lbz2 -liconv \
  -framework CoreFoundation -framework Security -framework CoreServices -framework CoreText \
  -framework CoreGraphics -framework SystemConfiguration $(NATIVE_TEST_EXTRA_LIBS) -o $(2)
endef

test-native: check-native-test-deps all native-deps
	@mkdir -p build/test
	$(call LINK_NATIVE_TEST,$(NATIVE_TEST_SOURCES),build/test/test_input_native)
	$(call LINK_NATIVE_TEST,test/test_graph_parser_gtest.cpp,build/test/test_graph_parser_native)
	$(call LINK_NATIVE_TEST,test/test_input_https_pull_gtest.cpp lib/https_pull.cpp,build/test/test_input_https_pull_native,-DLAMBDA_HTTPS_PULL_TESTING,build/lib/liblambda-input.a build/lib/liblambda-lib.a)
	$(PYTHON) test/run_native_tests.py ./build/test/test_input_native
	$(PYTHON) test/run_native_tests.py ./build/test/test_input_https_pull_native
	./build/test/test_graph_parser_native

clean:
	rm -rf build

-include $(addsuffix .d,$(LIB_OBJECTS) $(DATA_OBJECTS))
