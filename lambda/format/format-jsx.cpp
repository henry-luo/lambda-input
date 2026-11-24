#include "format.h"
#include "format-utils.h"
#include "lib/stringbuf.h"
#include "../core/mark_reader.hpp"
#include <ctype.h>

// Forward declarations
static void format_jsx_element(StringBuf* sb, const ElementReader& elem);
static void format_jsx_item(StringBuf* sb, const ItemReader& item);

// ---------------------------------------------------------------------------
// Text and attribute value helpers (gen-neutral, operate on String*)
// ---------------------------------------------------------------------------

// Format JSX text content with proper escaping
static void format_jsx_text_content(StringBuf* sb, String* text) {
    if (!text || text->len == 0 || text->len > 10000) return;
    format_escaped_string(sb, text->chars, text->len,
        JSX_TEXT_ESCAPE_RULES, JSX_TEXT_ESCAPE_RULES_COUNT);
}

// Format JSX attribute value with escaping
static void format_jsx_attribute_value(StringBuf* sb, String* value) {
    if (!value || value->len == 0) return;

    stringbuf_append_char(sb, '"');
    format_escaped_string(sb, value->chars, value->len,
        JSX_ATTR_ESCAPE_RULES, JSX_ATTR_ESCAPE_RULES_COUNT);
    stringbuf_append_char(sb, '"');
}

// ---------------------------------------------------------------------------
// Attribute formatting (uses shape-level access via elem.element())
// ---------------------------------------------------------------------------

// Check if an element tagged "js" and format as {expression}
static bool try_format_js_expr(StringBuf* sb, const ElementReader& elem) {
    const char* tag = elem.tagName();
    if (!tag || strcmp(tag, "js") != 0) return false;

    stringbuf_append_char(sb, '{');
    if (elem.childCount() > 0) {
        ItemReader first = elem.childAt(0);
        if (first.isString()) {
            String* js = first.asString();
            if (js && js->len > 0 && js->len < 10000) {
                stringbuf_append_str_n(sb, js->chars, js->len);
            }
        }
    }
    stringbuf_append_char(sb, '}');
    return true;
}

// Format JSX attributes from element shape data
static void format_jsx_attributes(StringBuf* sb, const ElementReader& elem) {
    if (!elem.element() || !elem.element()->data) return;

    auto attrs = elem.attrs();
    const char* attr_name;
    size_t attr_length;
    ItemReader value;
    while (attrs.next(&attr_name, &value, nullptr, &attr_length)) {
        if (!attr_name) continue;

        // Skip internal markers
        if (str_eq_const(attr_name, attr_length, "is_component") ||
            str_eq_const(attr_name, attr_length, "self_closing")) {
            continue;
        }

        // Skip internal JSX type marker
        if (str_eq_const(attr_name, attr_length, "type") && value.isString()) {
            String* type_value = value.asString();
            if (type_value && str_eq_const(type_value->chars, type_value->len, "jsx_element")) {
                continue;
            }
        }

        stringbuf_append_char(sb, ' ');
        stringbuf_append_str_n(sb, attr_name, attr_length);

        if (value.isString()) {
            String* attr_value = value.asString();
            if (attr_value && !str_eq_const(attr_value->chars, attr_value->len, "true")) {
                stringbuf_append_char(sb, '=');
                format_jsx_attribute_value(sb, attr_value);
            }
        } else if (value.isElement()) {
            ElementReader expr = value.asElement();
            if (expr.tagName() && strcmp(expr.tagName(), "js") == 0) {
                stringbuf_append_char(sb, '=');
                try_format_js_expr(sb, expr);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Element dispatch
// ---------------------------------------------------------------------------

static void format_jsx_element(StringBuf* sb, const ElementReader& elem) {
    const char* tag = elem.tagName();
    if (!tag) return;

    // JSX fragment: <>...</>
    if (strcmp(tag, "jsx_fragment") == 0) {
        stringbuf_append_str(sb, "<>");
        for (int64_t i = 0; i < elem.childCount(); i++)
            format_jsx_item(sb, elem.childAt(i));
        stringbuf_append_str(sb, "</>");
        return;
    }

    // JS expression: {expr}
    if (try_format_js_expr(sb, elem)) return;

    // Regular JSX element
    stringbuf_append_format(sb, "<%s", tag);

    format_jsx_attributes(sb, elem);

    // Self-closing check
    ItemReader sc = elem.get_attr("self_closing");
    if (sc.isString()) {
        String* scv = sc.asString();
        if (scv && str_eq_const(scv->chars, scv->len, "true")) {
            stringbuf_append_str(sb, " />");
            return;
        }
    }

    stringbuf_append_char(sb, '>');

    // Children
    for (int64_t i = 0; i < elem.childCount(); i++)
        format_jsx_item(sb, elem.childAt(i));

    // Closing tag
    stringbuf_append_format(sb, "</%s>", tag);
}

// ---------------------------------------------------------------------------
// Item dispatch
// ---------------------------------------------------------------------------

static void format_jsx_item(StringBuf* sb, const ItemReader& item) {
    if (item.isNull()) return;

    if (item.isString()) {
        format_jsx_text_content(sb, item.asString());
    } else if (item.isElement()) {
        format_jsx_element(sb, item.asElement());
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

String* format_jsx(Pool* pool, Item root_item) {
    StringBuf* sb = stringbuf_new(pool);
    ItemReader root(root_item.to_const());
    format_jsx_item(sb, root);
    return stringbuf_to_string(sb);
}
