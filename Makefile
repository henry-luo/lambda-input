# Build the input archives directly from the same manifest consumed by Lambda.
PYTHON ?= python3
INPUT_DEPS_ROOT ?= deps
INPUT_EXTRA_CPPFLAGS ?=
INPUT_CPPFLAGS = -I. -Ilib/re2 -Ilib/font/woff2/include \
  -Ilib/image/libpng -Ilib/image/libjpeg-turbo/src -Ilib/image/libwebp/src \
  -Ilib/image/libavif/include -Ilib/image/libtiff/libtiff -Ilib/image/giflib \
  -Ilib/image/libnsbmp/include -I$(INPUT_DEPS_ROOT)/build_temp/image-config/include \
  -I$(INPUT_DEPS_ROOT)/build_temp/utf8proc -I$(INPUT_DEPS_ROOT)/mac-deps/curl-8.10.1/include \
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

.PHONY: all clean check-boundary
all: build/lib/liblambda-lib.a build/lib/liblambda-input.a

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

check-boundary: all
	$(PYTHON) scripts/check_boundary.py

clean:
	rm -rf build

-include $(addsuffix .d,$(LIB_OBJECTS) $(DATA_OBJECTS))
