#pragma once

#include "lambda/core/data.hpp"

struct MarkupTestInput {
    Input* input;
    char* source;
};

String* create_lambda_string(const char* text);
MarkupTestInput parse_test_markup(const char* content, const char* filename);
String* test_parse_to_format(const char* content, const char* filename, const char* output_format);
bool test_input_contains(const char* text, const char* needle);
