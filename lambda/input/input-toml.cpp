#include "input.hpp"
#include "lib/str.h"
#include "../io/mark_builder.hpp"
#include "../core/mark_reader.hpp"
#include "input-context.hpp"
#include "input-utils.hpp"
#include "source_tracker.hpp"
#include "lib/hashmap_typed.hpp"

using namespace lambda;

static const int TOML_MAX_DEPTH = 512;

// table declarations and inline values have different redefinition rules.
enum TomlNodeKind { TOML_UNTRACKED, TOML_IMPLICIT, TOML_DOTTED, TOML_DECLARED, TOML_INLINE, TOML_TABLE_ARRAY };
struct TomlNodeState { const void* node; TomlNodeKind kind; };
using TomlNodeMap = TypedHashMap<TomlNodeState,
    HashMapPointerMemberKeyOps<TomlNodeState, &TomlNodeState::node>>;
struct TomlContext : InputContext {
    struct hashmap* nodes;
    TomlContext(Input* input, const char* source) : InputContext(input, source, strlen(source)),
        nodes(TomlNodeMap::create(16)) { if (!nodes) addError("TOML table tracking allocation failed"); }
    ~TomlContext() { hashmap_free(nodes); }
    TomlNodeKind kind(const void* node) const {
        TomlNodeState key = {node, TOML_UNTRACKED};
        const TomlNodeState* found = nodes ? TomlNodeMap::get(nodes, key) : nullptr;
        return found ? found->kind : TOML_UNTRACKED;
    }
    void track(const void* node, TomlNodeKind kind) {
        if (nodes) {
            TomlNodeMap::set(nodes, {node, kind});
            if (TomlNodeMap::oom(nodes)) addError("TOML table tracking allocation failed");
        }
    }
};

static bool toml_put_unique(TomlContext& ctx, Map* owner, String* key, Item value) {
    if (ctx.kind(owner) == TOML_INLINE || MapReader(owner).has(key->chars)) {
        ctx.addError("TOML key is already defined or its table is sealed");
        return false;
    }
    ctx.builder.putToMap(lam::gc_borrow(owner), key, value);
    return true;
}

// underscores separate digits, and cannot border a sign, prefix, point or exponent.
static bool toml_scan_digits(const char** cursor, int base) {
    const char* first = *cursor;
    bool digit = false;
    while (**cursor) {
        int value = str_hex_val(**cursor);
        if (value >= 0 && value < base) { digit = true; (*cursor)++; }
        else if (**cursor == '_') {
            int next = str_hex_val((*cursor)[1]);
            if (!digit || next < 0 || next >= base) return false;
            digit = false;
            (*cursor)++;
        } else break;
    }
    return digit && *cursor > first;
}

// Forward declarations
static Item parse_value(TomlContext& ctx, const char **toml, int *line_num, int depth = 0);
static Array* parse_array(TomlContext& ctx, const char **toml, int *line_num, int depth = 0);
static Map* parse_inline_table(TomlContext& ctx, const char **toml, int *line_num, int depth = 0);
static bool parse_dotted_key(TomlContext& ctx, const char **toml, Map** owner, String** key, bool header = false);
static String* parse_bare_key(TomlContext& ctx, const char **toml);
static String* parse_quoted_key(TomlContext& ctx, const char **toml);
static String* parse_literal_key(TomlContext& ctx, const char **toml);
static String* parse_key(TomlContext& ctx, const char **toml);
static String* parse_basic_string(TomlContext& ctx, const char **toml);
static String* parse_literal_string(TomlContext& ctx, const char **toml);
static String* parse_multiline_basic_string(TomlContext& ctx, const char **toml, int *line_num);
static String* parse_multiline_literal_string(TomlContext& ctx, const char **toml, int *line_num);
static Item parse_number(TomlContext& ctx, const char **toml);
static bool handle_escape_sequence(TomlContext& ctx, StringBuf* sb, const char **toml, bool is_multiline, int *line_num);

// Common function to handle escape sequences in strings
// is_multiline: true for multiline basic strings, false for regular strings
static bool handle_escape_sequence(TomlContext& ctx, StringBuf* sb, const char **toml, bool is_multiline, int *line_num) {
    SourceTracker& tracker = ctx.tracker;

    if (**toml != '\\') return false;

    SourceLocation esc_loc = tracker.location();
    (*toml)++; // skip backslash
    tracker.advance(1);

    switch (**toml) {
        case '"': stringbuf_append_char(sb, '"'); break;
        case '\\': stringbuf_append_char(sb, '\\'); break;
        case 'b': stringbuf_append_char(sb, '\b'); break;
        case 'f': stringbuf_append_char(sb, '\f'); break;
        case 'n': stringbuf_append_char(sb, '\n'); break;
        case 'r': stringbuf_append_char(sb, '\r'); break;
        case 't': stringbuf_append_char(sb, '\t'); break;
        case 'u':
        case 'U': {
            int digits = **toml == 'u' ? 4 : 8;
            (*toml)++;
            tracker.advance(1);
            const char* hex_start = *toml;
            uint32_t codepoint = parse_hex_codepoint(toml, digits);
            tracker.advance(*toml - hex_start);
            // toml encodes scalar values, not UTF-16 surrogate pairs.
            if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
                ctx.addError(esc_loc, "TOML escape requires a Unicode scalar value and complete hex digits");
                return false;
            }
            stringbuf_append_utf8(sb, codepoint);
            (*toml)--;
            tracker.advance(-1);
        } break;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            if (is_multiline) {
                bool saw_line_break = false;
                // a continuation needs a real line break, not only horizontal whitespace.
                while (**toml && (**toml == ' ' || **toml == '\t' || **toml == '\n' || **toml == '\r')) {
                    if (**toml == '\r' && (*toml)[1] != '\n') {
                        ctx.addError(esc_loc, "Bare carriage return in TOML continuation");
                        return false;
                    }
                    if (**toml == '\n') {
                        saw_line_break = true;
                        if (line_num) (*line_num)++;
                    }
                    (*toml)++;
                    tracker.advance(1);
                }
                if (!saw_line_break) {
                    ctx.addError(esc_loc, "TOML continuation requires a line break");
                    return false;
                }
                (*toml)--; // Back up one since we'll increment at end
                tracker.advance(-1);
            } else {
                ctx.addError(esc_loc, "Invalid TOML string escape");
                return false;
            }
            break;
        default:
            ctx.addError(esc_loc, "Unknown TOML string escape");
            return false;
    }
    (*toml)++; // move to next character
    tracker.advance(1);
    return true;
}

static Item parse_prefixed_integer(TomlContext& ctx, const char** toml, int base,
                                   const char* kind, SourceLocation loc) {
    const char* start = *toml;
    const char* end = start + 2;
    if (!toml_scan_digits(&end, base)) {
        ctx.addError(loc, "Invalid TOML prefixed integer digits");
        return {.item = ITEM_ERROR};
    }
    // the shared exact integer reader takes contiguous digits.
    StringBuf* token = ctx.sb;
    stringbuf_reset(token);
    for (const char* ch = start; ch < end; ch++) if (*ch != '_') stringbuf_append_char(token, *ch);
    Item result = parse_prefixed_integer_value(ctx, token->str->chars, base, kind, loc, nullptr, true, true);
    if (result.item == ITEM_ERROR) return result;
    *toml = end;
    ctx.tracker.advance(end - start);
    return result;
}

static String* parse_toml_single_line_string(TomlContext& ctx, const char** toml,
                                             char quote, bool allow_escapes,
                                             const char* label, const char* content_name);

static void skip_line(const char **toml, int *line_num) {
    while (**toml && **toml != '\n') {
        (*toml)++;
    }
    if (**toml == '\n') {
        (*toml)++;
        (*line_num)++;
    }
}

static void skip_tab_pace_and_comments(const char **toml, int *line_num) {
    while (**toml) {
        if (**toml == ' ' || **toml == '\t') {
            (*toml)++;
        } else if (**toml == '#') {
            skip_line(toml, line_num);
        } else if (**toml == '\n' || **toml == '\r') {
            if (**toml == '\r' && *(*toml + 1) == '\n') {
                (*toml)++;
            }
            (*toml)++;
            (*line_num)++;
        } else {
            break;
        }
    }
}

static String* parse_bare_key(TomlContext& ctx, const char **toml) {
    SourceTracker& tracker = ctx.tracker;
    MarkBuilder* builder = &ctx.builder;
    StringBuf* sb = ctx.sb;
    stringbuf_reset(sb);
    const char *start = *toml;
    SourceLocation key_loc = tracker.location();

    // Bare keys can contain A-Z, a-z, 0-9, -, _ (including pure numeric keys)
    while (**toml && (str_char_is_alnum(**toml) || **toml == '-' || **toml == '_')) {
        (*toml)++;
        tracker.advance(1);
    }
    if (*toml == start) {
        ctx.addError(key_loc, "Empty bare key");
        return NULL; // no valid characters
    }

    int len = *toml - start;
    stringbuf_append_str_n(sb, start, (size_t)len);
    return builder->createString(sb->str->chars, sb->length);
}

static String* parse_quoted_key(TomlContext& ctx, const char **toml) {
    return parse_toml_single_line_string(ctx, toml, '"', true, "quoted key", "key");
}

static String* parse_literal_key(TomlContext& ctx, const char **toml) {
    return parse_toml_single_line_string(ctx, toml, '\'', false, "literal key", "key");
}

static String* parse_basic_string(TomlContext& ctx, const char **toml) {
    return parse_toml_single_line_string(ctx, toml, '"', true, "basic string", "string");
}

static String* parse_literal_string(TomlContext& ctx, const char **toml) {
    return parse_toml_single_line_string(ctx, toml, '\'', false, "literal string", "string");
}

static String* parse_toml_multiline_string(TomlContext& ctx, const char **toml,
                                           const char* delimiter, bool allow_escapes,
                                           const char* label, int *line_num) {
    SourceTracker& tracker = ctx.tracker;

    if (strncmp(*toml, delimiter, 3) != 0) return NULL;
    MarkBuilder* builder = &ctx.builder;
    StringBuf* sb = ctx.sb;
    stringbuf_reset(sb);
    SourceLocation str_loc = tracker.location();

    *toml += 3; // skip opening triple quotes
    tracker.advance(3);

    // skip optional newline right after opening quotes
    if (**toml == '\n') {
        (*toml)++;
        (*line_num)++;
    } else if (**toml == '\r' && *(*toml + 1) == '\n') {
        *toml += 2;
        tracker.advance(1);
        (*line_num)++;
    }

    bool found_closing = false;
    while (**toml) {
        if (strncmp(*toml, delimiter, 3) == 0) {
            int quotes = 3;
            while (quotes < 5 && (*toml)[quotes] == delimiter[0]) quotes++;
            for (int i = 3; i < quotes; i++) stringbuf_append_char(sb, delimiter[0]);
            *toml += quotes;
            tracker.advance(quotes);
            found_closing = true;
            break;
        }

        if (allow_escapes && **toml == '\\') {
            if (!handle_escape_sequence(ctx, sb, toml, true, line_num)) {
                return NULL;
            }
        } else {
            unsigned char ch = (unsigned char)**toml;
            if ((ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') || ch == 0x7F ||
                    (ch == '\r' && (*toml)[1] != '\n')) {
                ctx.addError(str_loc, "Unescaped control character in TOML multiline string");
                return NULL;
            }
            if (ch == '\r') {
                (*toml)++;
                tracker.advance(1);
                ch = '\n';
            }
            if (ch == '\n') (*line_num)++;
            stringbuf_append_char(sb, ch);
            (*toml)++;
            tracker.advance(1);
        }
    }

    if (!found_closing) {
        ctx.addError(str_loc, "Unterminated %s: missing closing %s", label, delimiter);
        return NULL;
    }

    return builder->createString(sb->str->chars, sb->length);
}

static String* parse_multiline_basic_string(TomlContext& ctx, const char **toml, int *line_num) {
    return parse_toml_multiline_string(ctx, toml, "\"\"\"", true,
        "multiline basic string", line_num);
}

static String* parse_multiline_literal_string(TomlContext& ctx, const char **toml, int *line_num) {
    return parse_toml_multiline_string(ctx, toml, "'''", false,
        "multiline literal string", line_num);
}

static String* parse_key(TomlContext& ctx, const char **toml) {
    if (**toml == '"') {
        return parse_quoted_key(ctx, toml);
    } else if (**toml == '\'') {
        return parse_literal_key(ctx, toml);
    } else {
        return parse_bare_key(ctx, toml);
    }
}

static String* parse_toml_single_line_string(TomlContext& ctx, const char** toml,
                                             char quote, bool allow_escapes,
                                             const char* label, const char* content_name) {
    SourceTracker& tracker = ctx.tracker;

    if (**toml != quote) return NULL;
    MarkBuilder* builder = &ctx.builder;
    StringBuf* sb = ctx.sb;
    stringbuf_reset(sb);
    SourceLocation str_loc = tracker.location();

    (*toml)++; // skip opening quote
    tracker.advance(1);

    while (**toml && **toml != quote) {
        if (allow_escapes && **toml == '\\') {
            if (!handle_escape_sequence(ctx, sb, toml, false, NULL)) {
                return NULL;
            }
            continue;
        }
        if (**toml == '\n') {
            ctx.addError(str_loc, "Unterminated %s: newline in %s", label, content_name);
            return NULL;
        }
        unsigned char ch = (unsigned char)**toml;
        if ((ch < 0x20 && ch != '\t') || ch == 0x7F) {
            ctx.addError(str_loc, "Unescaped control character in TOML string");
            return NULL;
        }
        stringbuf_append_char(sb, ch);
        (*toml)++;
        tracker.advance(1);
    }

    if (**toml == quote) {
        (*toml)++; // skip closing quote
        tracker.advance(1);
    } else {
        ctx.addError(str_loc, "Unterminated %s: missing closing quote", label);
        return NULL;
    }
    return builder->createString(sb->str->chars, sb->length);
}

static Item parse_number(TomlContext& ctx, const char **toml) {
    SourceTracker& tracker = ctx.tracker;

    const char *start = *toml;
    SourceLocation num_loc = tracker.location();

    // date and time tokens start with digits too; parse them before decimal numbers.
    if ((str_char_is_digit(start[0]) && str_char_is_digit(start[1]) && start[2] == ':') ||
        (str_char_is_digit(start[0]) && str_char_is_digit(start[1]) &&
         str_char_is_digit(start[2]) && str_char_is_digit(start[3]) &&
         start[4] == '-' && str_char_is_digit(start[5]) &&
         str_char_is_digit(start[6]) && start[7] == '-')) {
        const char* end = start;
        while (*end && *end != ',' && *end != ']' && *end != '}' &&
               *end != '#' && *end != '\r' && *end != '\n' &&
               *end != ' ' && *end != '\t') end++;
        size_t length = (size_t)(end - start);
        char* token = (char*)pool_calloc(ctx.input()->pool, length + 1);
        if (!token) return {.item = ITEM_ERROR};
        memcpy(token, start, length);
        char* parsed_end = NULL;
        DateTime* datetime = datetime_parse(ctx.input()->pool, token,
                                            DATETIME_PARSE_LAMBDA, &parsed_end);
        if (!datetime || parsed_end != token + length) {
            ctx.addError(num_loc, "Invalid TOML date or time");
            return {.item = ITEM_ERROR};
        }
        *toml = end;
        tracker.advance(length);
        return ctx.builder.createDateTime(*datetime);
    }

    const char* special = start;
    bool negative = *special == '-';
    if (*special == '+' || *special == '-') special++;
    if (strncmp(special, "inf", 3) == 0 || strncmp(special, "nan", 3) == 0) {
        *toml = special + 3;
        tracker.advance(*toml - start);
        return ctx.builder.createFloat(*special == 'n' ? NAN : negative ? -INFINITY : INFINITY);
    }

    // Handle hex, octal, binary integers
    if (**toml == '0' && *(*toml + 1) == 'x') {
        return parse_prefixed_integer(ctx, toml, 16, "hexadecimal", num_loc);
    }
    if (**toml == '0' && *(*toml + 1) == 'o') {
        return parse_prefixed_integer(ctx, toml, 8, "octal", num_loc);
    }
    if (**toml == '0' && *(*toml + 1) == 'b') {
        return parse_prefixed_integer(ctx, toml, 2, "binary", num_loc);
    }

    const char* temp = start;
    if (*temp == '+' || *temp == '-') temp++;
    const char* integer = temp;
    bool valid = toml_scan_digits(&temp, 10);
    if (*integer == '0' && temp - integer > 1) valid = false;
    if (*temp == '.') { temp++; valid = toml_scan_digits(&temp, 10) && valid; }
    if (*temp == 'e' || *temp == 'E') {
        temp++;
        if (*temp == '+' || *temp == '-') temp++;
        valid = toml_scan_digits(&temp, 10) && valid;
    }
    if (!valid) {
        ctx.addError(num_loc, "Invalid TOML numeric spelling");
        return {.item = ITEM_ERROR};
    }

    const char* token_end = temp;
    size_t token_len = (size_t)(token_end - start);
    Item number_item = parse_scanned_decimal_number(ctx, start, token_len, true, false);
    if (number_item.item == ITEM_NULL) {
        const char* msg = scanned_number_has_float_marker(start, token_len)
            ? "Invalid float number format"
            : "Invalid integer number format";
        ctx.addError(num_loc, msg);
        return {.item = ITEM_ERROR};
    }
    // decimal TOML numbers use the shared exact path so large IDs never pass through double.
    size_t consumed = token_end - *toml;
    *toml = token_end;
    tracker.advance(consumed);
    return number_item;
}

static Array* parse_array(TomlContext& ctx, const char **toml, int *line_num, int depth) {
    Input* input = ctx.input();

    if (**toml != '[') return NULL;
    if (depth >= TOML_MAX_DEPTH) {
        ctx.addError(ctx.tracker.location(), "Maximum TOML nesting depth (%d) exceeded", TOML_MAX_DEPTH);
        return NULL;
    }
    Array* arr = array_pooled(input->pool);
    if (!arr) return NULL;

    (*toml)++; // skip [
    skip_tab_pace_and_comments(toml, line_num);

    if (**toml == ']') {
        (*toml)++;
        return arr;
    }

    while (**toml) {
        Item value = parse_value(ctx, toml, line_num, depth + 1);
        if (value.item == ITEM_ERROR) {
            return NULL;
        }
        array_append(arr, value, input->pool);

        skip_tab_pace_and_comments(toml, line_num);

        if (**toml == ']') {
            (*toml)++;
            return arr;
        }
        if (**toml != ',') {
            return NULL;
        }
        (*toml)++; // skip comma
        skip_tab_pace_and_comments(toml, line_num);

        // Handle trailing comma
        if (**toml == ']') {
            (*toml)++;
            return arr;
        }
    }

    ctx.addError("Unterminated TOML array");
    return nullptr;
}

static Map* parse_inline_table(TomlContext& ctx, const char **toml, int *line_num, int depth) {
    SourceTracker& tracker = ctx.tracker;

    Input* input = ctx.input();

    if (**toml != '{') return NULL;
    if (depth >= TOML_MAX_DEPTH) {
        ctx.addError(ctx.tracker.location(), "Maximum TOML nesting depth (%d) exceeded", TOML_MAX_DEPTH);
        return NULL;
    }
    Map* mp = map_pooled(input->pool);
    if (!mp) return NULL;

    (*toml)++; // skip '{'
    tracker.advance(1);
    skip_tab_pace_and_comments(toml, line_num);

    if (**toml == '}') { // empty table
        (*toml)++;
        tracker.advance(1);
        ctx.track(mp, TOML_INLINE);
        return mp;
    }

    while (**toml) {
        Map* owner = mp;
        String* key = NULL;
        if (!parse_dotted_key(ctx, toml, &owner, &key)) {
            return NULL;
        }

        skip_tab_pace(toml);
        if (**toml != '=') {
            return NULL;
        }
        (*toml)++;
        tracker.advance(1);
        skip_tab_pace(toml);

        Item value = parse_value(ctx, toml, line_num, depth + 1);
        if (value.item == ITEM_ERROR) {
            return NULL;
        }

        if (!toml_put_unique(ctx, owner, key, value)) return NULL;

        skip_tab_pace_and_comments(toml, line_num);
        if (**toml == '}') {
            (*toml)++;
            tracker.advance(1);
            ctx.track(mp, TOML_INLINE);
            return mp;
        }
        if (**toml != ',') {
            return NULL;
        }
        (*toml)++;
        tracker.advance(1);
        skip_tab_pace_and_comments(toml, line_num);
    }
    ctx.addError("Unterminated TOML inline table");
    return nullptr;
}

static Item parse_value(TomlContext& ctx, const char **toml, int *line_num, int depth) {
    SourceTracker& tracker = ctx.tracker;

    if (depth >= TOML_MAX_DEPTH) {
        ctx.addError(ctx.tracker.location(), "Maximum TOML nesting depth (%d) exceeded", TOML_MAX_DEPTH);
        return {.item = ITEM_ERROR};
    }

    skip_tab_pace(toml);

    SourceLocation value_loc = tracker.location();
    switch (**toml) {
        case '{': {
            Map* table = parse_inline_table(ctx, toml, line_num, depth + 1);
            if (!table) {
                ctx.addError(value_loc, "Invalid inline table");
            }
            return table ? (Item){.item = (uint64_t)table} : (Item){.item = ITEM_ERROR};
        }
        case '[': {
            Array* array = parse_array(ctx, toml, line_num, depth + 1);
            if (!array) {
                ctx.addError(value_loc, "Invalid array");
            }
            return array ? (Item){.item = (uint64_t)array} : (Item){.item = ITEM_ERROR};
        }
        case '"': {
            String* str = NULL;
            if (strncmp(*toml, "\"\"\"", 3) == 0) {
                str = parse_multiline_basic_string(ctx, toml, line_num);
            } else {
                str = parse_basic_string(ctx, toml);
            }
            if (!str) {
                ctx.addError(value_loc, "Invalid string value");
                return (Item){.item = ITEM_ERROR};
            }
            return (Item){.item = s2it(str)};
        }
        case '\'': {
            String* str = NULL;
            if (strncmp(*toml, "'''", 3) == 0) {
                str = parse_multiline_literal_string(ctx, toml, line_num);
            } else {
                str = parse_literal_string(ctx, toml);
            }
            if (!str) {
                ctx.addError(value_loc, "Invalid literal string");
                return (Item){.item = ITEM_ERROR};
            }
            return (Item){.item = s2it(str)};
        }
        case 't':
            if (strncmp(*toml, "true", 4) == 0 && !str_char_is_alnum(*(*toml + 4))) {
                *toml += 4;
                tracker.advance(4);
                return {.item = b2it(true)};
            }
            ctx.addError(value_loc, "Invalid boolean: expected 'true'");
            return {.item = ITEM_ERROR};
        case 'f':
            if (strncmp(*toml, "false", 5) == 0 && !str_char_is_alnum(*(*toml + 5))) {
                *toml += 5;
                tracker.advance(5);
                return {.item = b2it(false)};
            }
            ctx.addError(value_loc, "Invalid boolean: expected 'false'");
            return {.item = ITEM_ERROR};
        case 'i':
            if (strncmp(*toml, "inf", 3) == 0) {
                return parse_number(ctx, toml);
            }
            ctx.addError(value_loc, "Invalid value starting with 'i'");
            return {.item = ITEM_ERROR};
        case 'n':
            if (strncmp(*toml, "nan", 3) == 0) {
                return parse_number(ctx, toml);
            }
            ctx.addError(value_loc, "Invalid value starting with 'n'");
            return {.item = ITEM_ERROR};
        case '-':
            if (*((*toml) + 1) == 'i' || *((*toml) + 1) == 'n' || str_char_is_digit(*((*toml) + 1))) {
                return parse_number(ctx, toml);
            }
            ctx.addError(value_loc, "Invalid negative number");
            return {.item = ITEM_ERROR};
        case '+':
            if (*((*toml) + 1) == 'i' || *((*toml) + 1) == 'n' || str_char_is_digit(*((*toml) + 1))) {
                return parse_number(ctx, toml);
            }
            ctx.addError(value_loc, "Invalid positive number");
            return {.item = ITEM_ERROR};
        default:
            if ((**toml >= '0' && **toml <= '9')) {
                return parse_number(ctx, toml);
            }
            ctx.addError(value_loc, "Unexpected character '%c' (0x%02X)", **toml, (unsigned char)**toml);
            return {.item = ITEM_ERROR};
    }
}

static Map* toml_child_map(TomlContext& ctx, Map* parent, String* key, TomlNodeKind kind) {
    if (ctx.kind(parent) == TOML_INLINE) return nullptr;
    MapReader reader(parent);
    Item existing = reader.get(key->chars).item();
    if (get_type_id(existing) == LMD_TYPE_MAP)
        return ctx.kind(existing.map) != TOML_INLINE ? existing.map : nullptr;
    if (get_type_id(existing) == LMD_TYPE_ARRAY && ctx.kind(existing.array) == TOML_TABLE_ARRAY &&
            existing.array->length > 0)
        return ArrayReader(existing.array).get(existing.array->length - 1).item().map;
    if (reader.has(key->chars)) return nullptr;
    Map* child = map_pooled(ctx.input()->pool);
    if (child) {
        ctx.builder.putToMap(lam::gc_borrow(parent), key, {.item = (uint64_t)child});
        ctx.track(child, kind);
    }
    return child;
}

// a dotted key adds its intermediate tables to the current table.
static bool parse_dotted_key(TomlContext& ctx, const char **toml, Map** owner, String** key, bool header) {
    *key = parse_key(ctx, toml);
    if (!*key) return false;
    skip_tab_pace(toml);
    while (**toml == '.') {
        (*toml)++;
        skip_tab_pace(toml);
        *owner = toml_child_map(ctx, *owner, *key, header ? TOML_IMPLICIT : TOML_DOTTED);
        if (!*owner) return false;
        *key = parse_key(ctx, toml);
        if (!*key) return false;
        skip_tab_pace(toml);
    }
    return true;
}

static Map* parse_table_header(TomlContext& ctx, const char **toml, Map* root) {
    bool array_table = (*toml)[1] == '[';
    *toml += array_table ? 2 : 1;
    skip_tab_pace(toml);

    Map* parent = root;
    String* key = NULL;
    if (!parse_dotted_key(ctx, toml, &parent, &key, true)) return NULL;
    if (**toml != ']' || (array_table && (*toml)[1] != ']')) return NULL;
    *toml += array_table ? 2 : 1;

    if (ctx.kind(parent) == TOML_INLINE) return nullptr;
    if (!array_table) {
        Map* table = toml_child_map(ctx, parent, key, TOML_IMPLICIT);
        if (!table || ctx.kind(table) != TOML_IMPLICIT) return nullptr;
        ctx.track(table, TOML_DECLARED);
        return table;
    }

    Item existing = MapReader(parent).get(key->chars).item();
    Array* tables = NULL;
    if (!MapReader(parent).has(key->chars)) {
        tables = array_pooled(ctx.input()->pool);
        if (!tables) return NULL;
        ctx.track(tables, TOML_TABLE_ARRAY);
        ctx.builder.putToMap(lam::gc_borrow(parent), key, {.item = (uint64_t)tables});
    } else if (get_type_id(existing) == LMD_TYPE_ARRAY && ctx.kind(existing.array) == TOML_TABLE_ARRAY) {
        tables = existing.array;
    } else {
        return NULL;
    }
    Map* table = map_pooled(ctx.input()->pool);
    if (table) {
        array_append(tables, {.item = (uint64_t)table}, ctx.input()->pool);
        ctx.track(table, TOML_DECLARED);
    }
    return table;
}

void parse_toml(Input* input, const char* toml_string) {
    if (!toml_string || !*toml_string) {
        input->root = {.item = ITEM_NULL};
        return;
    }
    TomlContext ctx(input, toml_string);

    Map* root_map = map_pooled(input->pool);
    if (!root_map) { return; }
    input->root = {.item = (uint64_t)root_map};

    const char *toml = toml_string;
    int line_num = 1;

    // Current table context
    Map* current_table = root_map;

    while (*toml) {
        skip_tab_pace_and_comments(&toml, &line_num);
        if (!*toml) break;

        // Check for table header
        if (*toml == '[') {
            Map* section_map = parse_table_header(ctx, &toml, root_map);
            if (!section_map) {
                ctx.addError(ctx.tracker.location(), "Invalid table header");
            } else {
                current_table = section_map;
            }
            skip_tab_pace(&toml);
            if (*toml && *toml != '\r' && *toml != '\n' && *toml != '#')
                ctx.addError("Unexpected content after TOML table header");
            skip_line(&toml, &line_num);
            continue;
        }

        // Parse key-value pair
        SourceLocation key_loc = ctx.tracker.location();
        Map* owner = current_table;
        String* key = NULL;
        if (!parse_dotted_key(ctx, &toml, &owner, &key)) {
            ctx.addError(key_loc, "Invalid or empty key");
            skip_line(&toml, &line_num);
            continue;
        }

        skip_tab_pace(&toml);
        if (*toml != '=') {
            ctx.addError(ctx.tracker.location(), "Expected '=' after key '%.*s'", (int)key->len, key->chars);
            skip_line(&toml, &line_num);
            continue;
        }
        toml++; // skip '='

        Item value = parse_value(ctx, &toml, &line_num);
        if (value.item == ITEM_ERROR) {
            ctx.addError(ctx.tracker.location(), "Failed to parse value for key '%.*s'", (int)key->len, key->chars);
            skip_line(&toml, &line_num);
            continue;
        }

        skip_tab_pace(&toml);
        if (*toml && *toml != '\r' && *toml != '\n' && *toml != '#') {
            ctx.addError("Unexpected content after TOML value");
            skip_line(&toml, &line_num);
            continue;
        }

        toml_put_unique(ctx, owner, key, value);

        skip_line(&toml, &line_num);
    }

    if (ctx.hasErrors()) {
        ctx.logErrors();
    }
}
