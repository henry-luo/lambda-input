#pragma once

#include "lib/str.h"

// input keyword identity is independent of rendering and live control storage.
enum FormInputKind {
    FORM_INPUT_KIND_TEXT = 0, FORM_INPUT_KIND_PASSWORD, FORM_INPUT_KIND_EMAIL,
    FORM_INPUT_KIND_URL, FORM_INPUT_KIND_SEARCH, FORM_INPUT_KIND_TEL,
    FORM_INPUT_KIND_NUMBER, FORM_INPUT_KIND_CHECKBOX, FORM_INPUT_KIND_RADIO,
    FORM_INPUT_KIND_BUTTON, FORM_INPUT_KIND_SUBMIT, FORM_INPUT_KIND_RESET,
    FORM_INPUT_KIND_IMAGE, FORM_INPUT_KIND_HIDDEN, FORM_INPUT_KIND_RANGE,
    FORM_INPUT_KIND_FILE, FORM_INPUT_KIND_DATE, FORM_INPUT_KIND_TIME,
    FORM_INPUT_KIND_DATETIME_LOCAL, FORM_INPUT_KIND_MONTH, FORM_INPUT_KIND_WEEK,
    FORM_INPUT_KIND_COLOR, FORM_INPUT_KIND_COUNT,
};

struct HtmlInputTypeData {
    const char* keyword;
    bool auto_direction_value;
    bool uses_datalist;
};
inline constexpr HtmlInputTypeData HTML_INPUT_TYPES[] = {
    {"text", true, true}, {"password", true, false}, {"email", true, true},
    {"url", true, true}, {"search", true, true}, {"tel", true, true},
    {"number", false, true}, {"checkbox", false, false}, {"radio", false, false},
    {"button", true, false}, {"submit", true, false}, {"reset", true, false},
    {"image", false, false}, {"hidden", true, false}, {"range", false, true},
    {"file", false, false}, {"date", false, true}, {"time", false, true},
    {"datetime-local", false, true}, {"month", false, true}, {"week", false, true},
    {"color", false, true},
};
static_assert(sizeof(HTML_INPUT_TYPES) / sizeof(*HTML_INPUT_TYPES) == FORM_INPUT_KIND_COUNT);

inline FormInputKind form_input_kind(const char* type) {
    if (type && *type) {
        for (size_t i = 0; i < FORM_INPUT_KIND_COUNT; i++)
            if (str_icmp_cstr(type, HTML_INPUT_TYPES[i].keyword) == 0) return (FormInputKind)i;
    }
    return FORM_INPUT_KIND_TEXT;
}

inline bool html_input_has_auto_direction_value(const char* type) {
    return HTML_INPUT_TYPES[form_input_kind(type)].auto_direction_value;
}
