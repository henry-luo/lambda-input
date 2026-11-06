#pragma once

#include <stddef.h>
#include <stdint.h>

// shared native command vocabulary has no runtime or renderer ownership.
enum WebGlOp {
#define WEBGL_METHOD(name, js, signature, arity) WEBGL_##js,
#include "webgl_methods.def"
#undef WEBGL_METHOD
    WEBGL_OP_COUNT
};
static inline const char* webgl_op_name(WebGlOp op) {
    switch (op) {
#define WEBGL_METHOD(native, js, signature, arity) case WEBGL_##js: return #js;
#include "webgl_methods.def"
#undef WEBGL_METHOD
        default: return "unknown";
    }
}
struct WebGlCommand {
    WebGlOp op;
    double n[16];
    uint64_t resource[16];
    const void* data;
    size_t bytes;
    const char* text;
    bool compact_pixels; // DOM image rows ignore client-view unpack state
    bool source_dimensions; // DOM-source overload supplies the upload rectangle
};
enum WebGlReplyKind { WEBGL_VOID, WEBGL_NUMBER, WEBGL_BOOLEAN, WEBGL_STRING,
    WEBGL_NUMBERS, WEBGL_RESOURCE, WEBGL_LOCATION, WEBGL_ACTIVE_INFO, WEBGL_PRECISION };
struct WebGlReply {
    WebGlReplyKind kind;
    double n[16];
    unsigned count, resource_kind;
    uint64_t resource, generation, program;
    char text[8192];
    const char* source; // synchronous borrow copied before returning to script
};
struct WebGlOptions { bool alpha, depth, stencil, antialias, premultiplied_alpha, preserve; };

#define WEBGL_ENGINE_OPERATIONS(OP) \
    OP(uint64_t, create, (void* canvas, const WebGlOptions* options)) \
    OP(bool, call, (uint64_t canvas, const WebGlCommand* command, WebGlReply* reply)) \
    OP(bool, image, (uint64_t canvas, const WebGlCommand* command, void* source, WebGlReply* reply)) \
    OP(bool, extension_supported, (uint64_t canvas, const char* name)) \
    OP(void, error, (uint64_t canvas, unsigned error)) \
    OP(bool, resize, (void* canvas, unsigned width, unsigned height)) \
    OP(bool, has_context, (void* canvas)) \
    OP(bool, lose, (uint64_t canvas)) \
    OP(bool, restore, (uint64_t canvas))
