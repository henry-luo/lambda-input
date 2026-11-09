#include "test_input_test_helpers.hpp"
#include "lambda/input/input.hpp"
#include "lambda/format/format.h"
#include "lib/url.h"
#include <stdlib.h>
#include <string.h>

String* create_lambda_string(const char* text) {
    if (!text) return nullptr;
    size_t len = strlen(text);
    String* result = (String*)malloc(sizeof(String) + len + 1);
    if (!result) return nullptr;
    result->len = len;
    memcpy(result->chars, text, len + 1);
    return result;
}

MarkupTestInput parse_test_markup(const char* content, const char* filename) {
    String* type = create_lambda_string("auto");
    Url* cwd = get_current_dir();
    Url* url = parse_url(cwd, filename);
    char* source = strdup(content);
    Input* input = input_from_source(source, url, type, nullptr);
    // The parser may borrow the input bytes until formatting completes.
    if (!input) { free(source); source = nullptr; }
    return {input, source};
}

String* test_parse_to_format(const char* content, const char* filename, const char* output_format) {
    MarkupTestInput parsed = parse_test_markup(content, filename);
    if (!parsed.input) return nullptr;
    String* type = create_lambda_string(output_format);
    String* formatted = format_data(parsed.input->root, type, nullptr, parsed.input->pool);
    free(parsed.source);
    return formatted;
}

bool test_input_contains(const char* text, const char* needle) {
    return text && needle && strstr(text, needle) != nullptr;
}
