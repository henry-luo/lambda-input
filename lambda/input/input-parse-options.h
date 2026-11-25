#ifndef LAMBDA_INPUT_PARSE_OPTIONS_H
#define LAMBDA_INPUT_PARSE_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
struct SecurityIOClient;
struct Input;
struct Item;

// The native MathML and AsciiMath readers are separate from TeX math.
static inline bool input_math_is_tex(const char* type, const char* flavor) {
    if (!type) return false;
    const char* selected = !strcmp(type, "math") ? flavor :
        !strncmp(type, "math-", 5) ? type + 5 : NULL;
    if (strcmp(type, "math") && strncmp(type, "math-", 5)) return false;
    return !selected || (strcmp(selected, "ascii") && strcmp(selected, "asciimath") && strcmp(selected, "mathml"));
}

typedef struct InputScriptParser {
    void* context;
    // The result belongs to input, including when it is a fragment of another root.
    bool (*read)(void* context, struct Input* input, const char* type,
        const char* flavor, const char* source, size_t length, struct Item* result);
} InputScriptParser;

// Parse options beyond type and flavor.
typedef struct InputParseOptions {
    bool source_positions;  // parse({sourcepos: true}): markup blocks carry their source lines
    bool embedded_math;     // markup: list math nodes for runtime parsing and display
    // latex/tex: the TeX expansion engine (Lambda_Design_TeX_Engine)
    bool tex_ini;           // tex: primitives only, no LaTeX kernel
    const char* tex_base;   // document path or URL; beside-document files resolve from it
    const char* const* tex_adapters;  // package/class names answered by script adapters
    int tex_adapter_count;
    const char* const* tex_raw_commands;  // adapter commands whose arguments stay unexpanded
    int tex_raw_command_count;
    bool xml_preserve_whitespace; // xml text fragments can carry meaningful leading/trailing spaces.
    const char* const* tex_chaptered;  // class profiles whose kernel defines \chapter
    int tex_chaptered_count;
    // Borrowed only for this synchronous parse; never retained by Input/cache.
    const struct SecurityIOClient* io;
    const InputScriptParser* script_parser;
} InputParseOptions;

#endif
