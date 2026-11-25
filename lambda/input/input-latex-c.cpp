// Native AsciiMath input parser.
//
// The parser deliberately keeps the scanner small: one cursor, recursive
// groups, and MarkBuilder values.  It has no CST or private AST dependency.

#include "input-context.hpp"
#include "input-latex-scanner.h"
#include "input-latex-tables.h"
#include "input-parsers.h"
#include "input-parse-options.h"
#include "../io/mark_builder.hpp"
#include "lib/log.h"
#include "lib/str.h"
#include "lib/strbuf.h"
#include "lib/shell.h"
#include "lib/url.h"
#include "lib/memtrack.h"
#include <ctype.h>
#include <string.h>

using lambda::InputContext;

namespace {

static bool starts_with(const char* s, size_t n, size_t p, const char* needle) {
    return latex_scan_starts_with(s, n, p, needle);
}

static size_t latex_verbatim_end(const char* source, size_t length, size_t cursor) {
    if (cursor < length && source[cursor] == '*') cursor++;
    if (cursor >= length) return length;
    char delimiter = source[cursor++];
    while (cursor < length && source[cursor] != delimiter) cursor++;
    return cursor < length ? cursor + 1 : cursor;
}

static bool item_present(Item item) {
    return item.item != ITEM_NULL;
}

static bool is_big_operator(const char* name) {
    return strcmp(name, "sum") == 0 || strcmp(name, "prod") == 0 ||
        strcmp(name, "coprod") == 0 || strcmp(name, "int") == 0 ||
        strcmp(name, "iint") == 0 || strcmp(name, "iiint") == 0 ||
        strcmp(name, "oint") == 0 || strcmp(name, "intop") == 0 || strcmp(name, "oiint") == 0 ||
        strcmp(name, "oiiint") == 0 || strcmp(name, "lim") == 0 ||
        strcmp(name, "limsup") == 0 || strcmp(name, "liminf") == 0 ||
        strcmp(name, "sup") == 0 || strcmp(name, "inf") == 0 ||
        strcmp(name, "max") == 0 || strcmp(name, "min") == 0 ||
        strcmp(name, "det") == 0 || strcmp(name, "gcd") == 0;
}

static bool is_text_command(const char* name) {
    return strcmp(name, "text") == 0 || strcmp(name, "mbox") == 0 ||
        strcmp(name, "hbox") == 0 || strcmp(name, "textrm") == 0 ||
        strcmp(name, "textit") == 0 || strcmp(name, "textbf") == 0 ||
        strcmp(name, "texttt") == 0 || strcmp(name, "textsf") == 0 ||
        strcmp(name, "textsc") == 0 || strcmp(name, "emph") == 0;
}

static bool is_literal_text_escape(const char* name) {
    return name[0] && name[1] == '\0' && strchr("$%#&_{}", name[0]) != nullptr;
}

static bool is_style_command(const char* name) {
    return strcmp(name, "mathrm") == 0 || strcmp(name, "mathbf") == 0 ||
        strcmp(name, "boldsymbol") == 0 ||
        strcmp(name, "mathit") == 0 || strcmp(name, "mathsf") == 0 || strcmp(name, "mathsfit") == 0 ||
        strcmp(name, "mathtt") == 0 || strcmp(name, "mathcal") == 0 ||
        strcmp(name, "mathbb") == 0 || strcmp(name, "mathfrak") == 0 ||
        strcmp(name, "mathscr") == 0 || strcmp(name, "mathnormal") == 0 ||
        strcmp(name, "operatorname") == 0 || strcmp(name, "displaystyle") == 0 ||
        strcmp(name, "textstyle") == 0 || strcmp(name, "scriptstyle") == 0 ||
        strcmp(name, "scriptscriptstyle") == 0;
}

static bool is_size_command(const char* name) {
    static const char* commands[] = {"tiny", "sixptsize", "scriptsize", "footnotesize", "small",
        "normalsize", "large", "Large", "LARGE", "huge", "Huge"};
    for (const char* command : commands) if (strcmp(name, command) == 0) return true;
    return false;
}

static bool is_math_accent(const char* name) {
    static const char* commands[] = {"hat", "bar", "vec", "dot", "ddot", "dddot", "ddddot",
        "tilde", "acute", "breve", "check", "grave", "widehat", "widetilde", "widecheck",
        "overline", "underline", "underbar", "overbrace", "underbrace", "overbracket", "underbracket",
        "overrightarrow", "Overrightarrow", "underrightarrow", "overleftarrow", "underleftarrow",
        "overleftrightarrow", "underleftrightarrow", "overleftharpoon", "overrightharpoon",
        "overgroup", "undergroup", "overlinesegment", "underlinesegment", "utilde", "mathring"};
    for (const char* command : commands) if (strcmp(name, command) == 0) return true;
    return false;
}

static bool is_extended_arrow(const char* name) {
    static const char* commands[] = {"xrightarrow", "xleftarrow", "xleftrightarrow", "xRightarrow",
        "xLeftarrow", "xLeftrightarrow", "xhookrightarrow", "xhookleftarrow", "xtwoheadrightarrow",
        "xtwoheadleftarrow", "xmapsto", "xrightleftharpoons", "xleftrightharpoons",
        "xrightleftarrows", "xrightequilibrium", "xleftequilibrium"};
    for (const char* command : commands) if (strcmp(name, command) == 0) return true;
    return false;
}

static const char* math_atom_class(const char* name) {
    static const struct { const char* command; const char* atom; } atoms[] = {
        {"mathord", "mord"}, {"mathbin", "mbin"}, {"mathrel", "mrel"},
        {"mathopen", "mopen"}, {"mathclose", "mclose"}, {"mathpunct", "mpunct"}, {"mathinner", "minner"}};
    for (const auto& atom : atoms) if (strcmp(name, atom.command) == 0) return atom.atom;
    return nullptr;
}

static bool is_spacing_command(const char* name) {
    return strcmp(name, "quad") == 0 || strcmp(name, "qquad") == 0 ||
        strcmp(name, "hspace") == 0 || strcmp(name, "vspace") == 0 ||
        strcmp(name, "hskip") == 0 || strcmp(name, "vskip") == 0 ||
        strcmp(name, "kern") == 0 || strcmp(name, "mkern") == 0;
}

static bool is_sized_delimiter_command(const char* name) {
    return strcmp(name, "big") == 0 || strcmp(name, "bigl") == 0 ||
        strcmp(name, "bigr") == 0 || strcmp(name, "bigm") == 0 ||
        strcmp(name, "Big") == 0 || strcmp(name, "Bigl") == 0 ||
        strcmp(name, "Bigr") == 0 || strcmp(name, "Bigm") == 0 ||
        strcmp(name, "bigg") == 0 || strcmp(name, "biggl") == 0 ||
        strcmp(name, "biggr") == 0 || strcmp(name, "biggm") == 0 ||
        strcmp(name, "Bigg") == 0 || strcmp(name, "Biggl") == 0 ||
        strcmp(name, "Biggr") == 0 || strcmp(name, "Biggm") == 0;
}

static bool is_box_command(const char* name) {
    return strcmp(name, "bbox") == 0 || strcmp(name, "boxed") == 0 ||
        strcmp(name, "fbox") == 0 || strcmp(name, "llap") == 0 ||
        strcmp(name, "rlap") == 0 || strcmp(name, "clap") == 0 ||
        strcmp(name, "mathllap") == 0 || strcmp(name, "mathrlap") == 0 ||
        strcmp(name, "mathclap") == 0;
}

static bool is_infix_fraction_command(const char* name) {
    return strcmp(name, "over") == 0 || strcmp(name, "atop") == 0 ||
        strcmp(name, "above") == 0 || strcmp(name, "choose") == 0 ||
        strcmp(name, "brace") == 0 || strcmp(name, "brack") == 0;
}

static bool is_supported_math_environment(const char* name) {
    static const char* environments[] = {"array", "matrix", "matrix*", "pmatrix", "pmatrix*",
        "bmatrix", "bmatrix*", "Bmatrix", "Bmatrix*", "vmatrix", "vmatrix*", "Vmatrix", "Vmatrix*",
        "cases", "rcases", "dcases", "aligned", "alignedat", "align", "align*", "alignat", "alignat*",
        "gathered", "gather", "gather*", "split", "equation", "equation*", "smallmatrix", "subarray", "CD", "IEEEeqnarray",
        // mathtools
        "psmallmatrix", "psmallmatrix*", "bsmallmatrix", "bsmallmatrix*", "Bsmallmatrix", "Bsmallmatrix*",
        "vsmallmatrix", "vsmallmatrix*", "Vsmallmatrix", "Vsmallmatrix*", "drcases", "cases*", "dcases*",
        "rcases*", "drcases*", "multlined", "lgathered", "rgathered"};
    for (const char* environment : environments) if (strcmp(name, environment) == 0) return true;
    return false;
}

static bool is_math_document_environment(const char* name) {
    if (is_math_environment(name)) return true;
    size_t length = strlen(name);
    if (length < 2 || length >= 96 || name[length - 1] != '*') return false;
    char base[96];
    memcpy(base, name, length - 1);
    base[length - 1] = '\0';
    return is_math_environment(base);
}

static void append_latex_content(MarkBuilder& builder, ElementBuilder& command, Array* arguments) {
    for (int64_t i = 0; i < arguments->length;) {
        int64_t end = i;
        while (end < arguments->length && get_type_id(arguments->items[end]) == LMD_TYPE_STRING) end++;
        if (end > i + 1) {
            // S2.6.1v2/S2.6.4: argument and math-token boundaries belong in arrays.
            ArrayBuilder run = builder.array();
            while (i < end) run.append(arguments->items[i++]);
            command.child(run.final());
        } else {
            command.child(arguments->items[i++]);
        }
    }
}


class DirectMathParser {
public:
    DirectMathParser(InputContext& context, const char* source, size_t length,
                     size_t source_offset, bool ascii, bool allow_infix = true, bool text_mode = false)
        : ctx_(context), builder_(context.builder), source_(source), length_(length),
          offset_(source_offset), position_(0), ascii_(ascii), allow_infix_(allow_infix), text_mode_(text_mode) {}

    Item parse(const char* expansion_error = nullptr) {
        ElementBuilder root = builder_.element("math");
        if (expansion_error) root.attr("error", builder_.createStringItem(expansion_error));
        parse_children(root, false);
        return root.final();
    }

    void parse_into(ElementBuilder& parent, bool matrix_mode) {
        parse_children(parent, matrix_mode);
    }

private:
    InputContext& ctx_;
    MarkBuilder& builder_;
    const char* source_;
    size_t length_;
    size_t offset_;
    size_t position_;
    bool ascii_;
    bool allow_infix_;
    bool text_mode_;
    bool text_columns_ = false;  // cells after a row's first & are text (starred cases)

    void error(const char* message) {
        ctx_.tracker.seek(offset_ + position_);
        ctx_.addError(ctx_.location(), "latex_math_c: %s", message);
    }

    void skip_space() {
        while (position_ < length_) {
            char c = source_[position_];
            if (c == '%' ) {
                while (position_ < length_ && source_[position_] != '\n' && source_[position_] != '\r') position_++;
                continue;
            }
            if (!isspace((unsigned char)c)) break;
            position_++;
        }
    }

    bool consume_group_span(char open, char close, size_t* content_start, size_t* content_end) {
        size_t end = latex_scan_group_end(source_, length_, position_, open, close,
                                          content_start, content_end);
        if (end != 0) {
            position_ = end;
            return true;
        }
        if (open == '{') {
            // MathLive keeps an unterminated command argument as recoverable
            // input (for example, `\\rule{` still produces its default rule).
            // Preserve the remaining source instead of rejecting the formula.
            *content_start = position_ < length_ ? position_ + 1 : length_;
            *content_end = length_;
            position_ = length_;
            return true;
        }
        // A bare unmatched '[' is punctuation, not an empty optional group.
        return false;
    }

    Item parse_group() {
        size_t begin = 0, end = 0;
        if (!consume_group_span('{', '}', &begin, &end)) return ItemNull;
        return parse_group_contents(begin, end);
    }

    Item parse_group_contents(size_t begin, size_t end) {
        DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_);
        ElementBuilder group = builder_.element("group");
        nested.parse_into(group, false);
        return group.final();
    }

    Item parse_brack_group() {
        size_t begin = 0, end = 0;
        if (!consume_group_span('[', ']', &begin, &end)) return ItemNull;
        DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_);
        ElementBuilder group = builder_.element("brack_group");
        nested.parse_into(group, false);
        return group.final();
    }

    Item parse_paren_script_group() {
        size_t begin = 0, end = 0;
        if (!consume_group_span('(', ')', &begin, &end)) return ItemNull;
        DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_);
        ElementBuilder group = builder_.element("group");
        nested.parse_into(group, false);
        return group.final();
    }

    Item parse_script_arg() {
        skip_space();
        if (position_ >= length_) {
            error("missing script argument");
            return ItemNull;
        }
        if (text_mode_) return parse_text_arg();
        if (source_[position_] == '{') return parse_group();
        if (ascii_ && source_[position_] == '(') return parse_paren_script_group();
        if (!ascii_) {
            // TeX takes precisely one token for an unbraced argument: in
            // `\\frac57`, 5 is the numerator and 7 is the denominator.
            // parse_primary() would consume the full numeric or word run.
            if (source_[position_] == '\\') return parse_command();
            if (isdigit((unsigned char)source_[position_]) || isalpha((unsigned char)source_[position_])) {
                return builder_.createStringItem(source_ + position_++, 1);
            }
            return parse_punctuation_or_operator();
        }
        return parse_primary();
    }

    Item parse_optional_script_arg() {
        skip_space();
        if (position_ >= length_) return ItemNull;
        return parse_script_arg();
    }

    Item parse_color_content() {
        skip_space();
        if (position_ >= length_ || source_[position_] != '{') return parse_script_arg();
        size_t begin = 0, end = 0;
        if (!consume_group_span('{', '}', &begin, &end)) return ItemNull;
        size_t first = begin;
        size_t last = end;
        while (first < last && isspace((unsigned char)source_[first])) first++;
        while (last > first && isspace((unsigned char)source_[last - 1])) last--;
        if (last - first >= 2 && source_[first] == '$' && source_[last - 1] == '$') {
            // Color commands inherit text-mode `$...$` switches.  The markers
            // delimit the embedded math; MathLive does not emit them as glyphs.
            return parse_group_contents(first + 1, last - 1);
        }
        return parse_group_contents(begin, end);
    }

    Item parse_dimension_arg() {
        skip_space();
        if (position_ >= length_) return ItemNull;
        if (source_[position_] == '{') {
            size_t begin = 0;
            size_t end = 0;
            if (!consume_group_span('{', '}', &begin, &end)) return ItemNull;
            return builder_.createStringItem(source_ + begin, end - begin);
        }
        size_t begin = position_;
        // an unbraced TeX dimension ends after its unit, even when math follows immediately.
        while (position_ < length_ && (source_[position_] == '+' || source_[position_] == '-')) {
            position_++;
            skip_space();
        }
        bool number = false;
        while (position_ < length_ && (isdigit((unsigned char)source_[position_]) || source_[position_] == '.')) {
            number = true;
            position_++;
        }
        if (number) {
            skip_space();
            if (position_ + 1 < length_ && isalpha((unsigned char)source_[position_]) &&
                isalpha((unsigned char)source_[position_ + 1])) position_ += 2;
            return builder_.createStringItem(source_ + begin, position_ - begin);
        }
        position_ = begin;
        while (position_ < length_ && !isspace((unsigned char)source_[position_])) position_++;
        return builder_.createStringItem(source_ + begin, position_ - begin);
    }

    Item parse_atom_with_scripts() {
        Item base = parse_primary();
        if (!item_present(base)) return base;
        Item sub = ItemNull;
        Item sup = ItemNull;
        const char* modifier = nullptr;
        for (;;) {
            skip_space();
            if (position_ < length_ && source_[position_] == '\'') {
                ElementBuilder primes = builder_.element("group");
                if (item_present(sup)) primes.child(sup);
                while (position_ < length_ && source_[position_] == '\'') {
                    position_++;
                    ElementBuilder prime = builder_.element("symbol_command");
                    prime.attr("name", builder_.createStringItem("prime"));
                    primes.child(prime.final());
                }
                sup = primes.final();
                continue;
            }
            if (position_ < length_ && source_[position_] == '\\') {
                char modifier_name[96];
                char modifier_full[104];
                size_t end = latex_scan_command(source_, length_, position_, modifier_name,
                                                sizeof(modifier_name), modifier_full,
                                                sizeof(modifier_full));
                if (end != 0 && (strcmp(modifier_name, "limits") == 0 ||
                                 strcmp(modifier_name, "nolimits") == 0)) {
                    position_ = end;
                    modifier = strcmp(modifier_name, "limits") == 0 ? "limits" : "nolimits";
                    continue;
                }
            }
            if (position_ >= length_ || (source_[position_] != '_' && source_[position_] != '^')) break;
            char marker = source_[position_++];
            Item value = parse_script_arg();
            if (marker == '_') sub = value;
            else if (item_present(sup)) {
                ElementBuilder combined = builder_.element("group");
                combined.child(sup);
                combined.child(value);
                sup = combined.final();
            } else sup = value;
        }
        if (!item_present(sub) && !item_present(sup)) return base;
        ElementBuilder result = builder_.element("subsup");
        result.attr("base", base);
        if (item_present(sub)) result.attr("sub", sub);
        if (item_present(sup)) result.attr("sup", sup);
        if (modifier != nullptr) result.attr("modifier", builder_.createStringItem(modifier));
        return result.final();
    }

    Item parse_primary() {
        skip_space();
        if (position_ >= length_) return ItemNull;
        char c = source_[position_];
        if (c == '{') return parse_group();
        // Bare brackets are math delimiters. Optional arguments are consumed
        // only by the commands that declare them (e.g. \sqrt and \color).
        if (c == '[') return parse_punctuation_or_operator();
        if (c == '\\') return parse_command();
        if ((unsigned char)c >= 0x80) {
            size_t start = position_;
            while (position_ < length_ && (unsigned char)source_[position_] >= 0x80) position_++;
            ElementBuilder elem = builder_.element("unicode_text");
            elem.attr("value", builder_.createStringItem(source_ + start, position_ - start));
            return elem.final();
        }
        if (c == '#' || c == '!') {
            size_t start = position_++;
            while (position_ < length_ &&
                   (source_[position_] == '#' || source_[position_] == '!' ||
                    isdigit((unsigned char)source_[position_]))) position_++;
            // Color-literal punctuation and digits stay in one upright atom;
            // splitting them would introduce binary-operator spacing.
            ElementBuilder elem = builder_.element("raw_math_text");
            elem.attr("value", builder_.createStringItem(source_ + start, position_ - start));
            return elem.final();
        }
        if (c == '.' && position_ + 2 < length_ && source_[position_ + 1] == '.' &&
            source_[position_ + 2] == '.') {
            // MathLive keeps a literal three-dot run in one upright atom.
            position_ += 3;
            ElementBuilder elem = builder_.element("raw_math_text");
            elem.attr("value", builder_.createStringItem("..."));
            return elem.final();
        }
        if (isdigit((unsigned char)c) || (c == '.' && position_ + 1 < length_ && isdigit((unsigned char)source_[position_ + 1]))) {
            size_t start = position_++;
            while (position_ < length_ && (isdigit((unsigned char)source_[position_]) || source_[position_] == '.')) position_++;
            return builder_.createStringItem(source_ + start, position_ - start);
        }
        if (isalpha((unsigned char)c)) {
            size_t start = position_++;
            // TeX math makes each bare letter its own ordinary atom. ASCII
            // math keeps multi-letter identifiers (for example, `alpha`).
            if (ascii_) {
                while (position_ < length_ && isalpha((unsigned char)source_[position_])) position_++;
            }
            return builder_.createStringItem(source_ + start, position_ - start);
        }
        return parse_punctuation_or_operator();
    }

    Item parse_punctuation_or_operator() {
        size_t start = position_++;
        char c = source_[start];
        const char* tag = "punctuation";
        if (c == '&') {
            return builder_.createSymbolItem("col_sep");
        }
        if (c == ';' && position_ < length_ && source_[position_] == ';') {
            position_++;
            return builder_.createSymbolItem("row_sep");
        }
        if (c == '-' && position_ < length_ && source_[position_] == '>') {
            position_++;
            tag = "relation";
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '=') {
            tag = (c == '=') ? "relation" : "operator";
        }
        else if (c == '<' || c == '>') {
            tag = "relation";
            if (position_ < length_ && source_[position_] == '=') position_++;
        } else if (c == '!' && position_ < length_ && source_[position_] == '=') {
            position_++;
            tag = "relation";
        }
        ElementBuilder elem = builder_.element(tag);
        elem.attr("value", builder_.createStringItem(source_ + start, position_ - start));
        return elem.final();
    }

    bool read_command(char* name, size_t name_capacity, char* full, size_t full_capacity) {
        size_t end = latex_scan_command(source_, length_, position_, name,
                                         name_capacity, full, full_capacity);
        if (end == 0) return false;
        position_ = end;
        return true;
    }

    Item parse_command() {
        char name[96];
        char full[104];
        if (!read_command(name, sizeof(name), full, sizeof(full))) return ItemNull;
        if (name[0] == '\0') return ItemNull;
        if (name[0] == ' ' || name[0] == '\t' || name[0] == '\n') return builder_.createStringItem(" ");
        // parse_primary sends control symbols here; \\ is a row break, never a printed backslash.
        if (strcmp(name, "\\") == 0 || strcmp(name, "cr") == 0)
            return builder_.createSymbolItem("row_sep");
        if (!allow_infix_ && is_infix_fraction_command(name)) return ItemNull;

        if (strcmp(name, "verb") == 0) {
            size_t after = latex_verbatim_end(source_, length_, position_);
            bool starred = position_ < length_ && source_[position_] == '*';
            if (starred) position_++;
            size_t begin = position_ < length_ ? position_ + 1 : length_;
            char delimiter = position_ < length_ ? source_[position_++] : '\0';
            position_ = after > begin && source_[after - 1] == delimiter ? after - 1 : after;
            ElementBuilder elem = builder_.element("verbatim");
            elem.attr("value", builder_.createStringItem(source_ + begin, position_ - begin));
            elem.attr("starred", builder_.createBool(starred));
            elem.attr("delimiter", builder_.createStringItem(&delimiter, 1));
            if (position_ < length_) position_++;
            return elem.final();
        }
        if (strcmp(name, "raisebox") == 0 || strcmp(name, "reflectbox") == 0 ||
            strcmp(name, "mathreflectbox") == 0 || strcmp(name, "vcenter") == 0 ||
            strcmp(name, "cancel") == 0 || strcmp(name, "bcancel") == 0 ||
            strcmp(name, "xcancel") == 0 || strcmp(name, "cancelto") == 0 || strcmp(name, "sout") == 0 ||
            strcmp(name, "pmb") == 0 || strcmp(name, "phase") == 0 || strcmp(name, "angl") == 0 ||
            strcmp(name, "angln") == 0 || strcmp(name, "anglr") == 0 || strcmp(name, "anglk") == 0) {
            ElementBuilder elem = builder_.element("box_transform");
            elem.attr("cmd", builder_.createStringItem(full));
            if (strcmp(name, "raisebox") == 0) {
                elem.attr("raise", parse_dimension_arg());
                // optional height/depth override the raised box's reported extents.
                static const char* extents[] = {"height", "depth"};
                for (const char* attr : extents) {
                    skip_space();
                    size_t begin = 0, end = 0;
                    if (position_ < length_ && source_[position_] == '[' && consume_group_span('[', ']', &begin, &end))
                        elem.attr(attr, builder_.createStringItem(source_ + begin, end - begin));
                }
            }
            bool text_box = strcmp(name, "raisebox") == 0 || strcmp(name, "reflectbox") == 0;
            // \cancelto{value}{expression}: the value labels the arrow's tip
            if (strcmp(name, "cancelto") == 0) elem.attr("value", parse_script_arg());
            if (strlen(name) == 5 && strncmp(name, "angl", 4) == 0) {
                elem.attr("cmd", builder_.createStringItem("\\angl"));
                elem.attr("body", builder_.createStringItem(name + 4, 1));
            } else elem.attr("body", text_box ? parse_text_arg() : parse_script_arg());
            return elem.final();
        }
        if (strcmp(name, "substack") == 0) {
            size_t begin = 0, end = 0;
            skip_space();
            ElementBuilder elem = builder_.element("environment");
            elem.attr("name", builder_.createStringItem("subarray"));
            elem.attr("columns", builder_.createStringItem("c"));
            if (consume_group_span('{', '}', &begin, &end)) {
                ElementBuilder body = builder_.element("env_body");
                DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_);
                nested.parse_into(body, true);
                elem.attr("body", body.final());
            }
            return elem.final();
        }
        if (strcmp(name, "ce") == 0 || strcmp(name, "pu") == 0) {
            // mhchem's argument is chemistry notation, not math: keep it raw for
            // lambda.doc.math's mhchem profile
            skip_space();
            size_t begin = 0, end = 0;
            ElementBuilder elem = builder_.element("chemistry");
            elem.attr("cmd", builder_.createStringItem(full));
            if (consume_group_span('{', '}', &begin, &end))
                elem.attr("source", builder_.createStringItem(source_ + begin, end - begin));
            return elem.final();
        }
        if (strcmp(name, "tag") == 0) {
            ElementBuilder elem = builder_.element("equation_tag");
            bool starred = position_ < length_ && source_[position_] == '*';
            if (starred) position_++;
            elem.attr("starred", builder_.createBool(starred));
            elem.attr("body", parse_text_arg());
            return elem.final();
        }
        if (strcmp(name, "nonumber") == 0 || strcmp(name, "notag") == 0 ||
            strcmp(name, "nobreak") == 0 || strcmp(name, "allowbreak") == 0 || strcmp(name, "newline") == 0) {
            ElementBuilder elem = builder_.element("layout_control");
            elem.attr("cmd", builder_.createStringItem(full));
            return elem.final();
        }
        if (strcmp(name, "hline") == 0 || strcmp(name, "hdashline") == 0) {
            ElementBuilder elem = builder_.element("array_rule");
            elem.attr("cmd", builder_.createStringItem(full));
            return elem.final();
        }
        // LaTeX 2.09 math font switches; \cal still selects the calligraphic alphabet
        if (strcmp(name, "rm") == 0 || strcmp(name, "it") == 0 || strcmp(name, "bf") == 0 ||
            strcmp(name, "sf") == 0 || strcmp(name, "tt") == 0 || strcmp(name, "cal") == 0) {
            ElementBuilder elem = builder_.element("font_switch");
            elem.attr("cmd", builder_.createStringItem(full));
            return elem.final();
        }
        if (strcmp(name, "bmod") == 0 || strcmp(name, "pmod") == 0 || strcmp(name, "mod") == 0 || strcmp(name, "pod") == 0) {
            ElementBuilder elem = builder_.element("mod_command");
            elem.attr("cmd", builder_.createStringItem(full));
            if (strcmp(name, "bmod") != 0) elem.attr("body", parse_script_arg());
            return elem.final();
        }
        if (strcmp(name, "genfrac") == 0) {
            ElementBuilder elem = builder_.element("genfrac");
            static const char* attrs[] = {"left_delim", "right_delim", "thickness", "style"};
            for (const char* attr : attrs) {
                skip_space();
                if (strcmp(attr, "left_delim") == 0 || strcmp(attr, "right_delim") == 0)
                    elem.attr(attr, parse_delimiter_token());
                else elem.attr(attr, parse_dimension_arg());
            }
            elem.attr("numer", parse_script_arg());
            elem.attr("denom", parse_script_arg());
            return elem.final();
        }
        if (strcmp(name, "Set") == 0 || strcmp(name, "Braket") == 0) {
            ElementBuilder elem = builder_.element("delimiter_group");
            elem.attr("left", builder_.createStringItem(strcmp(name, "Set") == 0 ? "\\{" : "\\langle"));
            elem.attr("right", builder_.createStringItem(strcmp(name, "Set") == 0 ? "\\}" : "\\rangle"));
            elem.attr("body", parse_script_arg());
            return elem.final();
        }
        static const char* colors[] = {"red", "green", "blue", "cyan", "magenta", "yellow", "black", "white", "purple", "orange", "gray"};
        for (const char* color : colors) if (strcmp(name, color) == 0) {
            ElementBuilder elem = builder_.element("color_command");
            elem.attr("cmd", builder_.createStringItem("\\textcolor"));
            elem.attr("color_raw", builder_.createStringItem(color));
            elem.attr("content", parse_script_arg());
            return elem.final();
        }
        if (strcmp(name, "fcolorbox") == 0) {
            ElementBuilder elem = builder_.element("color_command");
            elem.attr("cmd", builder_.createStringItem(full));
            static const char* attrs[] = {"border_color", "color_raw"};
            for (const char* attr : attrs) {
                size_t begin = 0, end = 0;
                skip_space();
                if (consume_group_span('{', '}', &begin, &end))
                    elem.attr(attr, builder_.createStringItem(source_ + begin, end - begin));
            }
            elem.attr("content", parse_text_arg());
            return elem.final();
        }

        if (strcmp(name, "frac") == 0 || strcmp(name, "dfrac") == 0 ||
            strcmp(name, "tfrac") == 0 || strcmp(name, "cfrac") == 0) {
            Item alignment = ItemNull;
            skip_space();
            if (strcmp(name, "cfrac") == 0 && position_ < length_ && source_[position_] == '[') alignment = parse_brack_group();
            Item numer = parse_script_arg();
            Item denom = parse_script_arg();
            ElementBuilder elem = builder_.element("fraction");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(alignment)) elem.attr("options", alignment);
            if (item_present(numer)) elem.attr("numer", numer);
            if (item_present(denom)) elem.attr("denom", denom);
            return elem.final();
        }
        if (strcmp(name, "nicefrac") == 0 || strcmp(name, "sfrac") == 0) {
            // nicefrac's \nicefrac[font]{a}{b} and xfrac's \sfrac[instance]{a}[separator]{b}
            // are slanted fractions; the font and instance select faces Lambda does not ship
            skip_space();
            if (position_ < length_ && source_[position_] == '[') parse_brack_group();
            Item numer = parse_script_arg();
            skip_space();
            Item separator = ItemNull;
            if (position_ < length_ && source_[position_] == '[') separator = parse_brack_group();
            Item denom = parse_script_arg();
            ElementBuilder elem = builder_.element("fraction");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(numer)) elem.attr("numer", numer);
            if (item_present(denom)) elem.attr("denom", denom);
            if (item_present(separator)) elem.attr("separator", separator);
            return elem.final();
        }
        if (strcmp(name, "binom") == 0 || strcmp(name, "dbinom") == 0 || strcmp(name, "tbinom") == 0) {
            Item top = parse_script_arg();
            Item bottom = parse_script_arg();
            ElementBuilder elem = builder_.element("binomial");
            elem.attr("cmd", builder_.createStringItem(full));
            // Fraction rendering shares the numerator/denominator contract for
            // bar and no-bar forms; `top`/`bottom` left binomials empty.
            if (item_present(top)) elem.attr("numer", top);
            if (item_present(bottom)) elem.attr("denom", bottom);
            return elem.final();
        }
        if (strcmp(name, "sqrt") == 0) {
            Item index = ItemNull;
            skip_space();
            if (position_ < length_ && source_[position_] == '[') index = parse_brack_group();
            Item radicand = parse_script_arg();
            ElementBuilder elem = builder_.element("radical");
            if (item_present(index)) elem.attr("index", index);
            if (item_present(radicand)) elem.attr("radicand", radicand);
            return elem.final();
        }
        if (is_extended_arrow(name)) {
            Item lower = ItemNull;
            skip_space();
            if (position_ < length_ && source_[position_] == '[') lower = parse_brack_group();
            Item upper = parse_script_arg();
            ElementBuilder elem = builder_.element("extended_arrow");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(upper)) elem.attr("upper", upper);
            if (item_present(lower)) elem.attr("lower", lower);
            return elem.final();
        }
        if (strcmp(name, "includegraphics") == 0) {
            ElementBuilder elem = builder_.element("image_command");
            elem.attr("cmd", builder_.createStringItem(full));
            size_t begin = 0, end = 0;
            skip_space();
            if (position_ < length_ && source_[position_] == '[' &&
                consume_group_span('[', ']', &begin, &end))
                elem.attr("options", builder_.createStringItem(source_ + begin, end - begin));
            skip_space();
            // paths and option values are raw source, not math identifiers or subscripts.
            if (position_ < length_ && source_[position_] == '{' &&
                consume_group_span('{', '}', &begin, &end))
                elem.attr("src", builder_.createStringItem(source_ + begin, end - begin));
            else error("includegraphics requires a braced source");
            return elem.final();
        }
        if (strcmp(name, "mathop") == 0) {
            Item body = parse_script_arg();
            ElementBuilder elem = builder_.element("mathop");
            if (item_present(body)) elem.attr("body", body);
            return elem.final();
        }
        if (const char* atom = math_atom_class(name)) {
            Item body = parse_script_arg();
            ElementBuilder elem = builder_.element("math_atom");
            elem.attr("cmd", builder_.createStringItem(full));
            elem.attr("atom", builder_.createStringItem(atom));
            if (item_present(body)) elem.attr("body", body);
            return elem.final();
        }
        if (strcmp(name, "mathchoice") == 0) {
            ElementBuilder elem = builder_.element("mathchoice");
            // all four branches must survive parsing; the current math style selects one later.
            static const char* styles[] = {"display", "text", "script", "scriptscript"};
            for (const char* style : styles) {
                Item branch = parse_script_arg();
                if (item_present(branch)) elem.attr(style, branch);
            }
            return elem.final();
        }
        if (strcmp(name, "stackrel") == 0 || strcmp(name, "overset") == 0 ||
            strcmp(name, "underset") == 0) {
            Item annotation = parse_script_arg();
            Item base = parse_script_arg();
            ElementBuilder elem = builder_.element("overunder_command");
            elem.attr("cmd", builder_.createStringItem(strcmp(name, "stackrel") == 0 ? "\\overset" : full));
            if (item_present(annotation)) elem.attr("annotation", annotation);
            if (item_present(base)) elem.attr("base", base);
            return elem.final();
        }
        if (strcmp(name, "left") == 0) return parse_delimiter_group(full);
        if (is_sized_delimiter_command(name)) {
            ElementBuilder elem = builder_.element("sized_delimiter");
            elem.attr("size", builder_.createStringItem(full));
            if (has_sized_delimiter_token()) elem.attr("delim", parse_delimiter_token());
            return elem.final();
        }
        if (strcmp(name, "begin") == 0) return parse_environment();
        if (strcmp(name, "text") == 0 || strcmp(name, "mbox") == 0 || strcmp(name, "hbox") == 0) {
            return parse_text_command(full);
        }
        if (is_text_command(name)) return parse_text_command(full);
        if (is_size_command(name)) {
            ElementBuilder elem = builder_.element("size_command");
            elem.attr("cmd", builder_.createStringItem(full));
            return elem.final();
        }
        if (is_style_command(name)) {
            ElementBuilder elem = builder_.element("style_command");
            elem.attr("cmd", builder_.createStringItem(full));
            if (strcmp(name, "operatorname") == 0 && position_ < length_ && source_[position_] == '*') {
                position_++;
                elem.attr("limits", builder_.createBool(true));
            }
            bool declaration = strcmp(name, "displaystyle") == 0 ||
                strcmp(name, "textstyle") == 0 || strcmp(name, "scriptstyle") == 0 ||
                strcmp(name, "scriptscriptstyle") == 0;
            if (!declaration) {
                Item arg = parse_script_arg();
                if (item_present(arg)) elem.attr("arg", arg);
            }
            return elem.final();
        }
        if (is_spacing_command(name)) {
            ElementBuilder elem = builder_.element("space_command");
            bool dimensional = strcmp(name, "hspace") == 0 || strcmp(name, "vspace") == 0 ||
                strcmp(name, "hskip") == 0 || strcmp(name, "vskip") == 0 ||
                strcmp(name, "kern") == 0 || strcmp(name, "mkern") == 0;
            if (dimensional) {
                elem.attr("cmd", builder_.createStringItem(full));
                Item value = parse_dimension_arg();
                if (item_present(value)) elem.attr("value", value);
            } else {
                elem.attr("value", builder_.createStringItem(full));
            }
            return elem.final();
        }
        if (strcmp(name, "color") == 0 || strcmp(name, "textcolor") == 0 || strcmp(name, "colorbox") == 0) {
            Item options = ItemNull;
            skip_space();
            if (position_ < length_ && source_[position_] == '[') options = parse_brack_group();
            size_t color_begin = 0;
            size_t color_end = 0;
            bool has_color_source = position_ < length_ && source_[position_] == '{' &&
                latex_scan_group_end(source_, length_, position_, '{', '}', &color_begin, &color_end) != 0;
            Item color = parse_script_arg();
            Item content = ItemNull;
            if (strcmp(name, "textcolor") == 0) content = parse_color_content();
            else if (strcmp(name, "colorbox") == 0) content = parse_text_arg();
            ElementBuilder elem = builder_.element(strcmp(name, "color") == 0 ?
                                                     "color_switch" : "color_command");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(options)) elem.attr("options", options);
            if (item_present(color)) elem.attr("color", color);
            // Color syntax is CSS-like data, so preserve punctuation and spaces
            // separately from its parsed math subtree.
            if (has_color_source) elem.attr("color_raw", builder_.createStringItem(source_ + color_begin, color_end - color_begin));
            if (item_present(content)) elem.attr("content", content);
            return elem.final();
        }
        if (strcmp(name, "phantom") == 0 || strcmp(name, "hphantom") == 0 ||
            strcmp(name, "vphantom") == 0 || strcmp(name, "smash") == 0) {
            // these commands change box dimensions and cannot use literal-command fallback.
            ElementBuilder elem = builder_.element("phantom_command");
            elem.attr("cmd", builder_.createStringItem(full));
            skip_space();
            if (strcmp(name, "smash") == 0 && position_ < length_ && source_[position_] == '[')
                elem.attr("options", parse_brack_group());
            Item content = parse_script_arg();
            if (item_present(content)) elem.attr("content", content);
            return elem.final();
        }
        if (is_box_command(name)) {
            Item options = ItemNull;
            skip_space();
            size_t options_begin = 0;
            size_t options_end = 0;
            bool has_options_source = false;
            if (strcmp(name, "bbox") == 0 && position_ < length_ && source_[position_] == '[') {
                has_options_source = latex_scan_group_end(source_, length_, position_, '[', ']', &options_begin, &options_end) != 0;
                options = parse_brack_group();
            }
            Item content = parse_optional_script_arg();
            ElementBuilder elem = builder_.element("box_command");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(options)) elem.attr("options", options);
            if (has_options_source) elem.attr("options_raw", builder_.createStringItem(source_ + options_begin, options_end - options_begin));
            if (item_present(content)) elem.attr("content", content);
            return elem.final();
        }
        if (is_math_accent(name)) {
            Item base = parse_optional_script_arg();
            ElementBuilder elem = builder_.element("accent");
            elem.attr("cmd", builder_.createStringItem(full));
            if (item_present(base)) elem.attr("base", base);
            return elem.final();
        }
        if (strcmp(name, "ne") == 0 || strcmp(name, "neq") == 0) {
            ElementBuilder elem = builder_.element("not_overlay");
            elem.attr("target", builder_.createStringItem("="));
            return elem.final();
        }
        if (is_greek_letter(name) || is_math_operator(name) || is_big_operator(name) || is_trig_function(name) || is_log_function(name)) {
            ElementBuilder elem = builder_.element(is_big_operator(name) || is_trig_function(name) || is_log_function(name) ? "command" : "symbol_command");
            elem.attr("name", builder_.createStringItem(name));
            return elem.final();
        }
        if (strcmp(name, "right") == 0) {
            ElementBuilder elem = builder_.element("malformed_right");
            elem.attr("delim", parse_delimiter_token());
            return elem.final();
        }
        if (strcmp(name, "end") == 0) {
            // An unmatched closing environment is an error token; leave its
            // following group for ordinary math parsing, as MathLive does.
            return builder_.element("malformed_end").final();
        }
        if (strcmp(name, "limits") == 0 || strcmp(name, "nolimits") == 0) {
            return builder_.createSymbolItem(name);
        }
        if (strcmp(name, ",") == 0 || strcmp(name, ":") == 0 || strcmp(name, ";") == 0 || strcmp(name, "!") == 0) {
            ElementBuilder elem = builder_.element("space_command");
            elem.attr("value", builder_.createStringItem(full));
            return elem.final();
        }
        if (strcmp(name, "not") == 0) {
            skip_space();
            if (position_ + 1 < length_ && source_[position_] == '{' && source_[position_ + 1] == '}') {
                // Keep the empty-argument spelling distinct: adjacent
                // `\not{}` targets are coalesced by MathLive's renderer.
                position_ += 2;
                return builder_.element("not_empty").final();
            }
            if (position_ < length_ && source_[position_] == '\\') {
                // negation wraps the parsed atom, so named relations retain their glyph and spacing.
                Item target = parse_command();
                if (item_present(target)) {
                    ElementBuilder elem = builder_.element("not_overlay");
                    elem.attr("target", target);
                    return elem.final();
                }
            }
            if (position_ < length_) {
                ElementBuilder elem = builder_.element("not_overlay");
                elem.attr("target", parse_script_arg());
                return elem.final();
            }
        }
        if (strlen(name) == 1 && strchr("{}[]()$%#&_", name[0])) {
            // Escaped punctuation is upright; combine adjacent compatible
            // escapes so MathLive serializes them as one CMR atom.
            char escaped[104];
            size_t escaped_len = 0;
            escaped[escaped_len++] = name[0];
            while (name[0] != '_' && position_ + 1 < length_ && source_[position_] == '\\' &&
                   strchr("{}[]()$%#&", source_[position_ + 1]) != nullptr) {
                escaped[escaped_len++] = source_[position_ + 1];
                position_ += 2;
            }
            escaped[escaped_len] = '\0';
            ElementBuilder elem = builder_.element("escaped_symbol");
            elem.attr("value", builder_.createStringItem(escaped));
            return elem.final();
        }
        ElementBuilder elem = builder_.element("command");
        elem.attr("name", builder_.createStringItem(name));
        bool preserves_raw_args = strcmp(name, "class") == 0 ||
            strcmp(name, "cssId") == 0 || strcmp(name, "htmlData") == 0;
        size_t arg_index = 0;
        for (;;) {
            skip_space();
            if (position_ < length_ && source_[position_] == '{') {
                size_t arg_begin = 0;
                size_t arg_end = 0;
                bool has_arg_source = preserves_raw_args &&
                    latex_scan_group_end(source_, length_, position_, '{', '}', &arg_begin, &arg_end) != 0;
                elem.child(parse_group());
                if (has_arg_source && arg_index == 0)
                    elem.attr("arg0_raw", builder_.createStringItem(source_ + arg_begin, arg_end - arg_begin));
                else if (has_arg_source && arg_index == 1)
                    elem.attr("arg1_raw", builder_.createStringItem(source_ + arg_begin, arg_end - arg_begin));
                arg_index++;
            } else if (position_ < length_ && source_[position_] == '[') {
                size_t before = position_;
                elem.child(parse_brack_group());
                // leave an unmatched bracket for the ordinary punctuation parser.
                if (position_ == before) break;
            } else break;
        }
        return elem.final();
    }

    Item parse_text_arg() {
        skip_space();
        size_t begin = 0, end = 0;
        if (position_ < length_ && source_[position_] == '{' && consume_group_span('{', '}', &begin, &end)) {
            DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_, true, true);
            ElementBuilder group = builder_.element("text_group");
            group.attr("source", builder_.createStringItem(source_ + begin, end - begin));
            nested.parse_text_into(group);
            return group.final();
        }
        if (position_ < length_ && source_[position_] == '\\') return parse_command();
        begin = position_;
        if (position_ < length_) position_++;
        while (position_ < length_ && ((unsigned char)source_[position_] & 0xC0) == 0x80) position_++;
        ElementBuilder text = builder_.element("raw_math_text");
        text.attr("value", builder_.createStringItem(source_ + begin, position_ - begin));
        return text.final();
    }

    void parse_text_into(ElementBuilder& parent) {
        while (position_ < length_) {
            char ch = source_[position_];
            if (ch == '%') {
                while (position_ < length_ && source_[position_] != '\n') position_++;
            } else if (ch == '{') parent.child(parse_text_arg());
            else if (ch == '$') {
                size_t begin = ++position_;
                // a text box's math switch owns its dollars, including escaped dollars inside math.
                while (position_ < length_ && source_[position_] != '$') {
                    if (source_[position_] == '\\' && position_ + 1 < length_) position_++;
                    position_++;
                }
                if (position_ == length_) {
                    // unmatched text dollars remain recoverable literal input.
                    parent.child(builder_.createStringItem("$"));
                    position_ = begin;
                    continue;
                }
                ElementBuilder math = builder_.element("embedded_math");
                DirectMathParser nested(ctx_, source_ + begin, position_ - begin, offset_ + begin, ascii_);
                nested.parse_into(math, false);
                parent.child(math.final());
                if (position_ < length_) position_++;
            } else if (ch == '\\') {
                size_t saved = position_;
                char name[96], full[104];
                read_command(name, sizeof(name), full, sizeof(full));
                const char* accent = nullptr;
                static const struct {const char* name; const char* accent;} accents[] = {
                    {"'", "acute"}, {"`", "grave"}, {"^", "hat"}, {"~", "tilde"},
                    {"=", "bar"}, {".", "dot"}, {"\"", "ddot"}, {"u", "breve"},
                    {"v", "check"}, {"H", "doubleacute"}, {"r", "mathring"}, {"c", "cedilla"}};
                for (const auto& entry : accents) if (strcmp(name, entry.name) == 0) accent = entry.accent;
                if (accent) {
                    ElementBuilder elem = builder_.element("accent");
                    elem.attr("cmd", builder_.createStringItem(accent));
                    elem.attr("base", parse_text_arg());
                    parent.child(elem.final());
                } else if (is_literal_text_escape(name) || strcmp(name, " ") == 0 || strcmp(name, ",") == 0) {
                    parent.child(builder_.createStringItem(strcmp(name, ",") == 0 ? " " : name));
                } else {
                    position_ = saved;
                    parent.child(parse_command());
                    // spaces after a control word terminate its name; they are not text glue.
                    if (isalpha((unsigned char)name[0]) && position_ == saved + strlen(full)) skip_space();
                }
            } else if (ch == '~' || isspace((unsigned char)ch)) {
                position_++;
                if (ch != '~') while (position_ < length_ && isspace((unsigned char)source_[position_])) position_++;
                parent.child(builder_.createStringItem(" "));
            } else {
                size_t begin = position_++;
                while (position_ < length_ && !strchr("{}$\\~%", source_[position_]) &&
                       !isspace((unsigned char)source_[position_])) position_++;
                parent.child(builder_.createStringItem(source_ + begin, position_ - begin));
            }
        }
    }

    Item parse_text_command(const char* full) {
        skip_space();
        size_t begin = 0, end = 0;
        bool has_content = consume_group_span('{', '}', &begin, &end);
        return text_command_span(full, has_content, begin, end);
    }

    // a text_command over source_[begin, end): a \text argument, or a text column
    Item text_command_span(const char* full, bool has_content, size_t begin, size_t end) {
        ElementBuilder elem = builder_.element("text_command");
        elem.attr("cmd", builder_.createStringItem(full));
        if (has_content) {
            if (memchr(source_ + begin, '\\', end - begin) ||
                memchr(source_ + begin, '$', end - begin) || memchr(source_ + begin, '{', end - begin) ||
                memchr(source_ + begin, '~', end - begin)) {
                DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_, true, true);
                ElementBuilder body = builder_.element("text_group");
                nested.parse_text_into(body);
                elem.attr("body", body.final());
            }
            if (!memchr(source_ + begin, '\\', end - begin)) {
                elem.attr("content", builder_.createStringItem(source_ + begin, end - begin));
            } else {
                StrBuf* content = strbuf_new_cap(end - begin + 1);
                if (!content) {
                    error("failed to allocate text command content");
                    return ItemNull;
                }
                for (size_t cursor = begin; cursor < end;) {
                    if (source_[cursor] != '\\') {
                        strbuf_append_char(content, source_[cursor++]);
                        continue;
                    }
                    char name[96], command[104];
                    size_t next = latex_scan_command(source_, end, cursor, name,
                        sizeof(name), command, sizeof(command));
                    if (is_literal_text_escape(name)) strbuf_append_char(content, name[0]);
                    else strbuf_append_str_n(content, source_ + cursor, next - cursor);
                    cursor = next;
                }
                // paint literal punctuation, retaining its TeX escapes for serialization.
                if (content->length != end - begin)
                    elem.attr("content_raw", builder_.createStringItem(source_ + begin, end - begin));
                elem.attr("content", builder_.createStringItem(content->str, content->length));
                strbuf_free(content);
            }
        }
        return elem.final();
    }

    Item parse_delimiter_token() {
        skip_space();
        if (position_ >= length_) return builder_.createStringItem(".");
        if (source_[position_] == '{') {
            if (position_ + 1 < length_ && source_[position_ + 1] == '}') {
                position_ += 2;
                return builder_.createStringItem("");
            }
            // MathLive accepts a braced delimiter spelling after \right while
            // retaining only its first delimiter token.
            size_t begin = ++position_;
            Item delim = parse_delimiter_token();
            if (position_ < length_ && source_[position_] == '}') position_++;
            else position_ = begin;
            return delim;
        }
        if (source_[position_] == '\\') {
            char name[96], full[104];
            read_command(name, sizeof(name), full, sizeof(full));
            return builder_.createStringItem(full);
        }
        return builder_.createStringItem(source_ + position_++, 1);
    }

    bool has_sized_delimiter_token() {
        skip_space();
        if (position_ >= length_) return false;
        char c = source_[position_];
        if (strchr("()[]{}|<>/.", c) != nullptr) return true;
        if (c != '\\') return false;
        char name[96];
        char full[104];
        size_t end = latex_scan_command(source_, length_, position_, name,
                                        sizeof(name), full, sizeof(full));
        if (end == 0) return false;
        // fontmath.ltx declares vertical arrows, floors and ceilings as delimiter tokens too.
        static const char* const delimiters[] = {
            "{", "}", "|", "vert", "Vert", "lvert", "rvert", "lVert", "rVert",
            "langle", "rangle", "lbrace", "rbrace", "lceil", "rceil", "lfloor", "rfloor",
            "uparrow", "downarrow", "updownarrow", "Uparrow", "Downarrow", "Updownarrow",
            "backslash", "arrowvert", "Arrowvert", "bracevert", "lmoustache", "rmoustache",
            // parenthesis aliases must be consumed as the delimiter, like bracket aliases.
            "lgroup", "rgroup", "lbrack", "rbrack", "lparen", "rparen"
        };
        for (const char* delimiter : delimiters) {
            if (strcmp(name, delimiter) == 0) return true;
        }
        return false;
    }

    Item parse_delimiter_group(const char* full) {
        skip_space();
        if (position_ < length_ && source_[position_] == '{') {
            // A braced token cannot be the delimiter after \left. Match
            // MathLive recovery: emit the invalid command then retain the
            // group's leading delimiter as ordinary math.
            position_++;
            ElementBuilder err = builder_.element("ERROR");
            err.child(builder_.createStringItem(full));
            ElementBuilder seq = builder_.element("_seq");
            seq.child(err.final());
            Item first = parse_primary();
            if (item_present(first)) seq.child(first);
            if (position_ < length_ && source_[position_] == '}') position_++;
            return seq.final();
        }
        Item left = parse_delimiter_token();
        ElementBuilder elem = builder_.element("delimiter_group");
        elem.attr("left", left);
        ArrayBuilder children = builder_.array();
        for (;;) {
            skip_space();
            size_t save = position_;
            if (position_ < length_ && source_[position_] == '\\') {
                char name[96], command[104];
                read_command(name, sizeof(name), command, sizeof(command));
                if (strcmp(name, "right") == 0) {
                    elem.attr("right", parse_delimiter_token());
                    append_latex_content(builder_, elem, children.final().array);
                    return elem.final();
                }
                if (strcmp(name, "middle") == 0) {
                    ElementBuilder middle = builder_.element("middle_delim");
                    // Invalid middle tokens are recovered as a null delimiter;
                    // the renderer drops the following offending atom.
                    if (has_sized_delimiter_token()) middle.attr("delim", parse_delimiter_token());
                    children.append(middle.final());
                    continue;
                }
                position_ = save;
            }
            Item child = parse_atom_with_scripts();
            if (!item_present(child)) break;
            children.append(child);
        }
        // MathLive recovers incomplete delimiter pairs with a null right side.
        elem.attr("right", builder_.createStringItem("."));
        append_latex_content(builder_, elem, children.final().array);
        return elem.final();
    }

    bool find_infix_fraction(size_t* command_begin, size_t* command_end,
                             char* command, size_t command_capacity) {
        size_t brace_depth = 0;
        for (size_t cursor = position_; cursor < length_;) {
            char c = source_[cursor];
            if (c == '{') {
                brace_depth++;
                cursor++;
                continue;
            }
            if (c == '}') {
                if (brace_depth > 0) brace_depth--;
                cursor++;
                continue;
            }
            if (c != '\\') {
                cursor++;
                continue;
            }

            char name[96];
            char full[104];
            size_t end = latex_scan_command(source_, length_, cursor, name,
                                            sizeof(name), full, sizeof(full));
            if (end == 0) {
                cursor++;
                continue;
            }
            if (strcmp(name, "begin") == 0) {
                cursor = skip_environment_span(end);
                continue;
            }
            if (strcmp(name, "verb") == 0) {
                cursor = latex_verbatim_end(source_, length_, end);
                continue;
            }
            if (brace_depth == 0 && is_infix_fraction_command(name)) {
                *command_begin = cursor;
                *command_end = end;
                size_t command_len = strlen(full);
                if (command_len >= command_capacity) command_len = command_capacity - 1;
                str_copy(command, command_capacity, full, command_len);
                return true;
            }
            cursor = end;
        }
        return false;
    }

    size_t skip_environment_span(size_t cursor) {
        while (cursor < length_ && isspace((unsigned char)source_[cursor])) cursor++;
        size_t begin = 0, end = 0;
        size_t after = latex_scan_group_end(source_, length_, cursor, '{', '}', &begin, &end);
        if (!after) return cursor;
        char name[96];
        size_t count = end - begin < sizeof(name) ? end - begin : sizeof(name) - 1;
        str_copy(name, sizeof(name), source_ + begin, count);
        size_t body_end = 0;
        return find_environment_end(name, after, &body_end, nullptr, nullptr);
    }

    size_t cell_boundary(size_t begin) {
        for (size_t cursor = begin; cursor < length_;) {
            char ch = source_[cursor];
            if (ch == '&') return cursor;
            if (ch == '%') {
                while (cursor < length_ && source_[cursor] != '\n') cursor++;
            } else if (ch == '{') {
                size_t end = latex_scan_group_end(source_, length_, cursor, '{', '}', nullptr, nullptr);
                cursor = end ? end : length_;
            } else if (ch == '\\') {
                char name[96], full[104];
                size_t end = latex_scan_command(source_, length_, cursor, name, sizeof(name), full, sizeof(full));
                if (strcmp(name, "\\") == 0 || strcmp(name, "cr") == 0) return cursor;
                cursor = strcmp(name, "begin") == 0 ? skip_environment_span(end) :
                    strcmp(name, "verb") == 0 ? latex_verbatim_end(source_, length_, end) : end;
            } else cursor++;
        }
        return length_;
    }

    Item parse_infix_fraction_side(size_t begin, size_t end, bool allow_infix) {
        DirectMathParser nested(ctx_, source_ + begin, end - begin, offset_ + begin, ascii_, allow_infix);
        ElementBuilder group = builder_.element("group");
        nested.parse_into(group, false);
        return group.final();
    }

    bool parse_infix_fraction(ElementBuilder& parent) {
        if (!allow_infix_ || ascii_ || position_ != 0) return false;
        size_t command_begin = 0;
        size_t command_end = 0;
        char command[104];
        if (!find_infix_fraction(&command_begin, &command_end, command, sizeof(command))) return false;

        // TeX scopes one infix fraction to its enclosing math list. Nested
        // braces start their own scope, but another unbraced infix operator is
        // recovery text rather than a recursively nested denominator fraction.
        ElementBuilder fraction = builder_.element("infix_frac");
        fraction.attr("cmd", builder_.createStringItem(command));
        fraction.attr("numer", parse_infix_fraction_side(position_, command_begin, false));
        if (strcmp(command, "\\above") == 0) {
            position_ = command_end;
            fraction.attr("thickness", parse_dimension_arg());
            command_end = position_;
        }
        fraction.attr("denom", parse_infix_fraction_side(command_end, length_, false));
        parent.child(fraction.final());
        position_ = length_;
        return true;
    }

    size_t find_environment_end(const char* name, size_t from, size_t* body_end,
                                bool* found, bool* recovered) {
        if (found) *found = false;
        if (recovered) *recovered = false;
        size_t name_len = strlen(name);
        size_t recovery_end = 0;
        size_t recovery_body_end = 0;
        size_t nested_depth = 0;
        for (size_t i = from; i < length_;) {
            if (source_[i] == '%') {
                while (i < length_ && source_[i] != '\n' && source_[i] != '\r') i++;
                continue;
            }
            if (source_[i] != '\\') { i++; continue; }
            size_t command_start = i;
            char command[96], full[104];
            size_t after_command = latex_scan_command(source_, length_, i, command, sizeof(command), full, sizeof(full));
            if (!after_command) { i++; continue; }
            i = after_command;
            if (strcmp(command, "begin") != 0 && strcmp(command, "end") != 0) continue;
            while (i < length_ && isspace((unsigned char)source_[i])) i++;
            size_t end_name_start = 0;
            size_t end_name_end = 0;
            size_t after_end = latex_scan_group_end(source_, length_, i, '{', '}',
                                                     &end_name_start, &end_name_end);
            if (!after_end) continue;
            i = after_end;
            // a nested environment's terminator cannot close the surrounding matrix.
            if (strcmp(command, "begin") == 0) { nested_depth++; continue; }
            if (nested_depth > 0) { nested_depth--; continue; }
            if (recovery_end == 0) {
                // MathLive recovers an unmatched environment by ending at the
                // first closing environment token rather than consuming it as
                // ordinary math content.
                recovery_body_end = command_start;
                recovery_end = after_end;
            }
            if (end_name_end - end_name_start == name_len &&
                memcmp(source_ + end_name_start, name, name_len) == 0) {
                if (body_end) *body_end = command_start;
                if (found) *found = true;
                return after_end;
            }
        }
        if (recovery_end != 0) {
            if (body_end) *body_end = recovery_body_end;
            if (found) *found = true;
            if (recovered) *recovered = true;
            return recovery_end;
        }
        return length_;
    }

    void parse_cd_into(ElementBuilder& parent) {
        bool vertical_cell = false;
        while (position_ < length_) {
            skip_space();
            if (position_ >= length_) break;
            if (starts_with(source_, length_, position_, "\\\\")) {
                position_ += 2;
                parent.child(builder_.createSymbolItem("row_sep"));
                vertical_cell = false;
            } else if (source_[position_] == '@' && position_ + 1 < length_) {
                size_t begin = position_++;
                char direction = source_[position_++];
                bool vertical = direction == 'A' || direction == 'V' || direction == '|';
                if (!vertical || vertical_cell) parent.child(builder_.createSymbolItem("col_sep"));
                if (vertical && vertical_cell) parent.child(builder_.createSymbolItem("col_sep"));
                ElementBuilder arrow = builder_.element("cd_arrow");
                arrow.attr("direction", builder_.createStringItem(&direction, 1));
                if (strchr("<>AV", direction)) {
                    static const char* attrs[] = {"upper", "lower"};
                    for (const char* attr : attrs) {
                        size_t label_begin = position_;
                        while (position_ < length_ && source_[position_] != direction) {
                            if (source_[position_] == '{') {
                                size_t end = latex_scan_group_end(source_, length_, position_, '{', '}', nullptr, nullptr);
                                position_ = end ? end : length_;
                            } else if (source_[position_] == '\\') {
                                char name[96], full[104];
                                position_ = latex_scan_command(source_, length_, position_, name, sizeof(name), full, sizeof(full));
                            } else position_++;
                        }
                        arrow.attr(attr, parse_group_contents(label_begin, position_));
                        if (position_ < length_) position_++;
                    }
                }
                arrow.attr("source", builder_.createStringItem(source_ + begin, position_ - begin));
                parent.child(arrow.final());
                if (!vertical) parent.child(builder_.createSymbolItem("col_sep"));
                vertical_cell = vertical;
            } else parent.child(parse_atom_with_scripts());
        }
    }

    Item parse_environment() {
        skip_space();
        size_t name_begin = 0, name_end = 0;
        // MathLive treats a bare `\begin` as an empty recoverable construct.
        // Do not turn that local omission into a fatal input parse error.
        if (position_ >= length_ || source_[position_] != '{') return ItemNull;
        if (!consume_group_span('{', '}', &name_begin, &name_end)) return ItemNull;
        char env_name[96];
        size_t env_len = name_end - name_begin;
        if (env_len >= sizeof(env_name)) env_len = sizeof(env_name) - 1;
        str_copy(env_name, sizeof(env_name), source_ + name_begin, env_len);
        if (!is_supported_math_environment(env_name)) {
            // MathLive ignores an unknown opening environment, then lets the
            // following `\end{...}` take its ordinary error-recovery path; the
            // name stays so the renderer can report the environment unsupported
            ElementBuilder ignored = builder_.element("ignored_begin");
            ignored.attr("name", builder_.createStringItem(env_name));
            return ignored.final();
        }
        ElementBuilder elem = builder_.element("environment");
        elem.attr("name", builder_.createStringItem(env_name));
        skip_space();
        if (strcmp(env_name, "alignedat") == 0 || strcmp(env_name, "alignat") == 0 || strcmp(env_name, "alignat*") == 0) {
            size_t begin = 0, end = 0;
            if (position_ < length_ && source_[position_] == '{' && consume_group_span('{', '}', &begin, &end))
                elem.attr("pairs", builder_.createStringItem(source_ + begin, end - begin));
        } else if (env_len > 0 && env_name[env_len - 1] == '*' && strstr(env_name, "matrix") != nullptr &&
                   position_ < length_ && source_[position_] == '[') {
            size_t begin = 0, end = 0;
            if (consume_group_span('[', ']', &begin, &end))
                elem.attr("alignment", builder_.createStringItem(source_ + begin, end - begin));
        }
        if (strcmp(env_name, "multlined") == 0 || strcmp(env_name, "lgathered") == 0 ||
            strcmp(env_name, "rgathered") == 0) {
            // mathtools' [position] and [width] options place the block in the
            // surrounding line; a laid-out formula has no line to place it in
            size_t begin = 0, end = 0;
            for (int option = 0; option < 2; option++) {
                skip_space();
                if (position_ >= length_ || source_[position_] != '[' ||
                    !consume_group_span('[', ']', &begin, &end)) break;
            }
        }
        if ((strcmp(env_name, "array") == 0 || strcmp(env_name, "subarray") == 0) &&
            position_ < length_ && source_[position_] == '{') {
            size_t columns_begin = 0, columns_end = 0;
            if (consume_group_span('{', '}', &columns_begin, &columns_end)) {
                elem.attr("columns", builder_.createStringItem(source_ + columns_begin,
                                                                 columns_end - columns_begin));
            }
        }
        size_t body_end = length_;
        bool found_end = false;
        bool recovered_end = false;
        size_t after_end = find_environment_end(env_name, position_, &body_end, &found_end,
                                                &recovered_end);
        if (recovered_end) {
            // A differently named closing token cannot close this environment.
            // Preserve the recoverable tree while exposing MathLive's error class.
            elem.attr("error", builder_.createStringItem("unbalanced-environment"));
        }
        const char* body_source = source_ + position_;
        size_t body_len = body_end >= position_ ? body_end - position_ : 0;
        ElementBuilder body = builder_.element("env_body");
        DirectMathParser nested(ctx_, body_source, body_len, offset_ + position_, ascii_);
        // mathtools' starred cases set the column after the first & as text
        size_t name_len = strlen(env_name);
        nested.text_columns_ = name_len > 6 && strcmp(env_name + name_len - 6, "cases*") == 0;
        if (strcmp(env_name, "CD") == 0) nested.parse_cd_into(body);
        else nested.parse_into(body, true);
        elem.attr("body", body.final());
        position_ = after_end;
        return elem.final();
    }

    void parse_children(ElementBuilder& parent, bool matrix_mode) {
        if (!matrix_mode && parse_infix_fraction(parent)) return;
        ArrayBuilder children = builder_.array();
        int column = 0;
        while (position_ < length_) {
            skip_space();
            if (position_ >= length_) break;
            if (matrix_mode && source_[position_] == '&') {
                position_++;
                column++;
                children.append(builder_.createSymbolItem("col_sep"));
                continue;
            }
            if (matrix_mode && (starts_with(source_, length_, position_, "\\\\") ||
                (starts_with(source_, length_, position_, "\\cr") &&
                 (position_ + 3 == length_ || !isalpha((unsigned char)source_[position_ + 3]))))) {
                column = 0;
                position_ += source_[position_ + 1] == '\\' ? 2 : 3;
                skip_space();
                size_t begin = 0, end = 0;
                if (position_ < length_ && source_[position_] == '[' &&
                    consume_group_span('[', ']', &begin, &end)) {
                    ElementBuilder separator = builder_.element("row_sep");
                    separator.attr("gap", builder_.createStringItem(source_ + begin, end - begin));
                    children.append(separator.final());
                } else children.append(builder_.createSymbolItem("row_sep"));
                continue;
            }
            if (matrix_mode) {
                if (source_[position_] == '\\') {
                    char name[96], full[104];
                    latex_scan_command(source_, length_, position_, name, sizeof(name), full, sizeof(full));
                    if (strcmp(name, "hline") == 0 || strcmp(name, "hdashline") == 0) {
                        // row rules are alignment material, outside the following cell's math list.
                        children.append(parse_command());
                        continue;
                    }
                }
                size_t end = cell_boundary(position_);
                if (text_columns_ && column > 0) {
                    size_t text_end = end;
                    while (text_end > position_ && isspace((unsigned char)source_[text_end - 1])) text_end--;
                    children.append(text_command_span("\\text", true, position_, text_end));
                    position_ = end;
                    continue;
                }
                DirectMathParser cell(ctx_, source_ + position_, end - position_, offset_ + position_, ascii_);
                ElementBuilder contents = builder_.element("group");
                cell.parse_into(contents, false);
                // each cell is its own math list, including infix fractions and declarations.
                Element* parsed_cell = contents.final().element;
                for (int64_t i = 0; i < parsed_cell->length; i++) children.append(parsed_cell->items[i]);
                position_ = end;
                continue;
            }
            Item child = parse_atom_with_scripts();
            if (item_present(child)) children.append(child);
            else if (position_ < length_) position_++;
        }
        append_latex_content(builder_, parent, children.final().array);
    }

};

} // namespace

extern "C" Item parse_math_direct_to_ast(Input* input, const char* math_source, size_t math_len, const char* flavor) {
    if (!input || !math_source) return ItemNull;
    bool ascii = flavor && (strcmp(flavor, "ascii") == 0 || strcmp(flavor, "asciimath") == 0);
    if (!ascii) {
        // TeX math is parsed by the runtime after source acquisition.
        input->parse_failed = true;
        input->parse_error_message = pool_strdup(input->pool, "TeX math requires a Lambda runtime host");
        return ItemError;
    }
    InputContext context(input, math_source, math_len);
    DirectMathParser parser(context, context.source(), context.source_length(), 0, true);
    Item result = parser.parse();
    if (context.hasErrors()) context.logErrors();
    return result;
}
