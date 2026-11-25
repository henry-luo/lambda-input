/**
 * path.c - Lambda Path implementation
 *
 * Paths are segmented symbols for file/URL navigation.
 * A path is a linked chain of segments from leaf to root.
 *
 * Example: file.etc.hosts
 *   Path("hosts") -> Path("etc") -> Path("file") -> ROOT_SENTINEL
 */

#include "lib/strbuf.h"
#include "lib/log.h"
#include "lib/mempool.h"
#include "lib/arraylist.h"
#include "lib/hashmap_helpers.h"
#include "lib/shell.h"
#include "lib/mem.h"
#include "../lambda.h"
#include "lambda-path.h"
#include "lib/file.h"
#include "lib/url.h"
#include "lib/escape.h"
// The active C ABI mirrors DateTime as its packed word.
extern DateTime* datetime_from_unix(Pool* pool, int64_t unix_timestamp);

#include <string.h>
#include <stdlib.h>

// Root-only spelling metadata. The normalized local authority is shared.
#define PATH_FLAG_FILE_SLASH 0x10u

static PathPoolProvider path_pool_provider = NULL;

static PathTypePrinter path_type_printer = NULL;
// shared with the runtime-only iteration build of this file below
PathEnumerationResolver g_path_enumeration_resolver = NULL;

void path_register_enumeration_resolver(PathEnumerationResolver resolver) {
    g_path_enumeration_resolver = resolver;
}

void path_register_type_printer(PathTypePrinter printer) {
    path_type_printer = printer;
}

void path_register_pool_provider(PathPoolProvider provider) {
    // Keep runtime TLS ownership above lambda-io; allocation always asks the
    // registered current-runtime provider rather than retaining a stale pool.
    path_pool_provider = provider;
}

Pool* path_get_pool(void) {
    return path_pool_provider ? path_pool_provider() : NULL;
}

// Root sentinel - parent of all scheme roots (has NULL parent itself)
static Path ROOT_SENTINEL = {
    LMD_TYPE_PATH,  // type_id
    0,              // flags
    NULL,           // name
    NULL            // parent
};

// Scheme names for string conversion
static const char* scheme_names[PATH_SCHEME_COUNT] = {
    "file",   // PATH_SCHEME_FILE
    "http",   // PATH_SCHEME_HTTP
    "https",  // PATH_SCHEME_HTTPS
    "sys",    // PATH_SCHEME_SYS
    ".",      // PATH_SCHEME_REL (relative)
    "..",     // PATH_SCHEME_PARENT (legacy only)
    "/",      // PATH_SCHEME_LOGICAL
    "temp",   // PATH_SCHEME_TEMP (in-memory documents, PTH44v2)
    "~doc"    // PATH_SCHEME_DOCUMENT (evaluation-supplied document)
};

static Path* path_root_of(Path* path) {
    if (!path) return NULL;
    while (path->parent && path->parent != &ROOT_SENTINEL) {
        path = path->parent;
    }
    return path;
}

static ArrayList* path_collect_ops(Path* path, Path* root) {
    ArrayList* ops = arraylist_new(8);
    if (!ops) return NULL;
    for (Path* p = path; p && p != root; p = p->parent) {
        if (!arraylist_append(ops, p)) { arraylist_free(ops); return NULL; }
    }
    return ops;
}

static bool path_is_child(Path* path) {
    return path && path->parent && path->parent != &ROOT_SENTINEL;
}

static Path* path_alloc_op(Pool* pool, Path* base, LPathSegmentType type,
                           const char* name, size_t len, int64_t int_value) {
    if (!pool || !base) return NULL;
    Path* path = (Path*)pool_calloc(pool, sizeof(Path));
    if (!path) return NULL;
    path->type_id = LMD_TYPE_PATH;
    path->flags = 0;
    PATH_SET_SEG_TYPE(path, type);
    path->parent = base;
    path->root_scheme = base->root_scheme;
    path->authority_kind = base->authority_kind;
    path->authority_name = base->authority_name;
    path->int_value = int_value;
    if (name && type == LPATH_SEG_NORMAL) {
        char* name_copy = pool_dup_n(pool, name, len);
        if (!name_copy) return NULL;
        path->name = name_copy;
        path->name_length = len;
    }
    return path;
}

// Compatibility lifecycle hooks: roots are now allocated in the requesting
// pool, so another evaluator's teardown cannot invalidate them (D6.4.2).
void path_init(void) {}
void path_reset(void) {}

/**
 * Get predefined root path for a scheme.
 */
Path* path_get_root(PathScheme scheme) {
    if (scheme < 0 || scheme >= PATH_SCHEME_COUNT) {
        log_error("path_get_root: invalid scheme %d", scheme);
        return NULL;
    }

    return path_new(path_get_pool(), scheme);
}

/**
 * Append a segment to a path.
 * Returns a new path with the segment appended.
 */
Path* path_append(Path* parent, const char* segment) {
    if (!segment) {
        log_error("path_append: NULL segment");
        return parent;
    }
    return path_append_len(parent, segment, strlen(segment));
}

/**
 * Append a segment to a path (with explicit length).
 * Copies the segment string into pool memory.
 */
Path* path_append_len(Path* parent, const char* segment, size_t len) {
    if (!parent) {
        log_error("path_append_len: NULL parent");
        return NULL;
    }
    if (!segment) {
        log_error("path_append_len: NULL segment");
        return parent;
    }

    Pool* pool = path_get_pool();
    if (!pool) {
        log_error("path_append_len: no registered pool provider");
        return NULL;
    }

    return path_alloc_op(pool, parent, LPATH_SEG_NORMAL, segment, len, 0);
}

Path* path_new_authority(Pool* pool, int scheme, const char* authority) {
    if (!pool || (authority ? !*authority : scheme != PATH_SCHEME_FILE)) return NULL;
    if (scheme < 0 || scheme >= PATH_SCHEME_COUNT) return NULL;
    Path* root = path_new(pool, scheme);
    if (!root) return NULL;
    if (!authority) {
        root->flags |= PATH_FLAG_FILE_SLASH;
        return root;
    }
    // RFC 8089 uses localhost for the empty authority on the current machine.
    if (scheme == PATH_SCHEME_FILE && strcmp(authority, "localhost") == 0) return root;
    root->authority_kind = PATH_AUTHORITY_NAMED;
    size_t len = strlen(authority);
    char* copy = pool_dup_n(pool, authority, len);
    root->authority_name = copy;
    return root;
}

/**
 * Get the scheme name for a path (file, http, https, sys, etc.)
 */
const char* path_get_scheme_name(Path* path) {
    if (!path) return NULL;
    Path* root = path_root_of(path);
    if (!root) return NULL;
    if ((PathScheme)root->root_scheme == PATH_SCHEME_LOGICAL) return "/";
    return root->name;
}

/**
 * Check if path is a root scheme (no segments after scheme).
 */
bool path_is_root(Path* path) {
    if (!path) return false;
    return path->parent == &ROOT_SENTINEL || PATH_GET_SEG_TYPE(path) == LPATH_SEG_ROOT;
}

/**
 * Get the scheme type of a path.
 * Walks to root and returns the scheme identifier.
 * Returns -1 if path is invalid.
 */
PathScheme path_get_scheme(Path* path) {
    if (!path) return (PathScheme)-1;
    return (PathScheme)path_root_of(path)->root_scheme;
}

/**
 * Check if a path is absolute (file, http, https, sys).
 * Returns false for relative (.) and parent (..) paths.
 */
bool path_is_absolute(Path* path) {
    PathScheme scheme = path_get_scheme(path);
    return scheme == PATH_SCHEME_FILE ||
           scheme == PATH_SCHEME_HTTP ||
           scheme == PATH_SCHEME_HTTPS ||
           scheme == PATH_SCHEME_SYS ||
           scheme == PATH_SCHEME_TEMP || scheme == PATH_SCHEME_DOCUMENT ||
           scheme == PATH_SCHEME_LOGICAL;
}

/**
 * Get the depth of a path (number of segments including scheme).
 */
int path_depth(Path* path) {
    int depth = 0;
    while (path && path->parent && path->parent != &ROOT_SENTINEL) {
        depth++;
        path = path->parent;
    }
    if (path && path->parent == &ROOT_SENTINEL) {
        depth++;  // count the scheme root itself
    }
    return depth;
}

static void path_print_name(StrBuf* out, const char* seg, size_t len) {
    const char* text = seg ? seg : "";
    bool needs_quote = len == 0;
    bool all_digits = len != 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < '0' || c > '9') all_digits = false;
        if (c < 32 || c == 127 || c == '\'' || c == '\\' ||
            c == '.' || c == ' ' || c == '@' || c == '#' ||
            c == '$' || c == '%' || c == '&' || c == '?' ||
            c == '=' || c == ':' || c == '-' || c == '*') {
            needs_quote = true;
        }
    }
    // Numeric-looking names must stay quoted so NameKey("1") cannot
    // canonicalize into the distinct IntKey(1) spelling.
    if (all_digits) needs_quote = true;
    if (!needs_quote) { strbuf_append_str_n(out, text, len); return; }
    strbuf_append_char(out, '\'');
    // NUL needs a source escape; the shared quoted helper handles the rest.
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '\0') continue;
        escape_append_c_quoted(out, text + start, i - start, '\'');
        strbuf_append_str(out, "\\u0000");
        start = i + 1;
    }
    escape_append_c_quoted(out, text + start, len - start, '\'');
    strbuf_append_char(out, '\'');
}

static void path_print_op(StrBuf* out, Path* op) {
    if (PATH_GET_SEG_TYPE(op) == LPATH_SEG_QUERY) {
        // S2.4.7: `?T` takes no dot and `.?T` does; equality and hashing use
        // this spelling, so the type's canonical print is its identity
        strbuf_append_str(out, (op->flags & PATH_FLAG_QUERY_SELF) ? ".?" : "?");
        if (path_type_printer) path_type_printer(op->query_type, out);
        else strbuf_append_str(out, "<type>");
        return;
    }
    strbuf_append_char(out, '.');
    switch (PATH_GET_SEG_TYPE(op)) {
        case LPATH_SEG_WILDCARD: strbuf_append_char(out, '*'); break;
        case LPATH_SEG_WILDCARD_REC: strbuf_append_str(out, "**"); break;
        case LPATH_SEG_DYNAMIC: strbuf_append_str(out, "<dynamic>"); break;
        case LPATH_SEG_PARENT: strbuf_append_str(out, "~~"); break;
        case LPATH_SEG_ROOT: strbuf_append_char(out, '/'); break;
        case LPATH_SEG_INT: strbuf_append_int64(out, op->int_value); break;
        default: path_print_name(out, op->name, op->name_length); break;
    }
}

/** Render normalized identity or the Lambda source spelling. */
static void path_print(Path* path, void* out_ptr, bool source) {
    StrBuf* out = (StrBuf*)out_ptr;
    if (!path || !out) return;

    Path* root = path_root_of(path);
    ArrayList* ops = path_collect_ops(path, root);
    if (!ops) return;

    PathScheme scheme = (PathScheme)root->root_scheme;
    if (scheme == PATH_SCHEME_LOGICAL) {
        strbuf_append_char(out, '/');
    } else if (scheme == PATH_SCHEME_REL) {
        // S16.9.4: the relative root is `\`, so a relative path prints `\.a.b`
        // and the empty one `\`. The escape is load-bearing, not cosmetic —
        // printed bare, `.rect` re-parses as part of a qualified name, so
        // `<svg \.rect>` would silently come back as tag `svg.rect`.
        // OS/filesystem rendering is unaffected: path_to_os_path builds its own
        // `./` form and never routes through here.
        strbuf_append_char(out, '\\');
    } else if (scheme == PATH_SCHEME_FILE) {
        if (source) strbuf_append_str(out, "\\.");
        strbuf_append_str(out, "file.");
        if (root->authority_kind == PATH_AUTHORITY_NAMED) {
            // Quote hostnames that are not one identifier token so a dotted or
            // hyphenated authority cannot be reparsed as path child keys.
            path_print_name(out, root->authority_name, strlen(root->authority_name));
        } else {
            bool spell_localhost = source && !(root->flags & PATH_FLAG_FILE_SLASH);
            strbuf_append_str(out, spell_localhost ? "localhost" : "/");
        }
    } else if (scheme == PATH_SCHEME_PARENT) {
        // same escape as PATH_SCHEME_REL above: bare `.~~` no longer parses.
        strbuf_append_str(out, "\\.~~");
    } else if (scheme == PATH_SCHEME_DOCUMENT) {
        if (source) strbuf_append_str(out, "\\.");
        strbuf_append_str(out, "~doc");
        // The evaluation assigns each supplied document its own identity.
        if (!source && root->authority_name) {
            strbuf_append_char(out, ':');
            strbuf_append_str(out, root->authority_name);
        }
    } else {
        if (source) strbuf_append_str(out, "\\.");
        strbuf_append_str(out, scheme_names[scheme]);
    }

    // every step follows its root with a dot, as `/.a` and `\.a` read
    for (int i = ops->length - 1; i >= 0; i--) {
        Path* step = (Path*)arraylist_get(ops, i);
        if (source && scheme == PATH_SCHEME_REL && i == ops->length - 1 &&
                PATH_GET_SEG_TYPE(step) == LPATH_SEG_NORMAL && step->name &&
                strlen(step->name) == step->name_length &&
                (strcmp(step->name, "file") == 0 || strcmp(step->name, "http") == 0 ||
                 strcmp(step->name, "https") == 0 || strcmp(step->name, "sys") == 0 ||
                 strcmp(step->name, "temp") == 0)) {
            strbuf_append_all(out, 3, "['", step->name, "']");
        } else {
            path_print_op(out, step);
        }
    }
    arraylist_free(ops);
}

void path_to_string(Path* path, void* out) { path_print(path, out, false); }
void path_to_source(Path* path, void* out) { path_print(path, out, true); }

bool path_to_http_url(Path* path, void* out_ptr) {
    StrBuf* out = (StrBuf*)out_ptr;
    if (!path || !out) return false;
    Path* root = path_root_of(path);
    PathScheme scheme = (PathScheme)root->root_scheme;
    if (scheme != PATH_SCHEME_HTTP && scheme != PATH_SCHEME_HTTPS) return false;
    ArrayList* ops = path_collect_ops(path, root);
    if (!ops || !ops->length) { if (ops) arraylist_free(ops); return false; }
    Path* host = (Path*)arraylist_get(ops, ops->length - 1);
    const char* authority = host->name;
    bool valid = PATH_GET_SEG_TYPE(host) == LPATH_SEG_NORMAL && authority && host->name_length;
    for (size_t i = 0; valid && i < host->name_length; i++) {
        unsigned char c = (unsigned char)authority[i];
        // An authority key cannot introduce credentials or URL delimiters.
        if (c <= 32 || c == 127 || strchr("/@?#\\%", c)) valid = false;
    }
    StrBuf* url = valid ? strbuf_new() : NULL;
    if (!url) valid = false;
    if (valid) {
        strbuf_append_str(url, scheme == PATH_SCHEME_HTTP ? "http://" : "https://");
        strbuf_append_str(url, authority);
        for (int i = ops->length - 2; valid && i >= 0; i--) {
            Path* step = (Path*)arraylist_get(ops, i);
            const char* name = step->name ? step->name : "";
            // URL dot segments navigate, so they cannot represent literal path keys.
            if (PATH_GET_SEG_TYPE(step) != LPATH_SEG_NORMAL ||
                memchr(name, '\0', step->name_length) ||
                (step->name_length == 1 && name[0] == '.') ||
                (step->name_length == 2 && name[0] == '.' && name[1] == '.')) {
                valid = false; break;
            }
            char* encoded = url_encode_component(name, step->name_length);
            if (!encoded) { valid = false; break; }
            strbuf_append_char(url, '/'); strbuf_append_str(url, encoded); mem_free(encoded);
        }
    }
    if (valid) {
        Url* parsed = url_parse(url->str);
        valid = parsed && url_is_valid(parsed) && parsed->host && parsed->host->len &&
            (!parsed->username || !parsed->username->len) && (!parsed->password || !parsed->password->len);
        if (parsed) url_destroy(parsed);
    }
    if (valid) strbuf_append_str(out, url->str);
    if (url) strbuf_free(url);
    arraylist_free(ops);
    return valid;
}

bool path_equal(Path* left, Path* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    StrBuf* left_buf = strbuf_new();
    StrBuf* right_buf = strbuf_new();
    path_to_string(left, left_buf);
    path_to_string(right, right_buf);
    bool equal = left_buf->length == right_buf->length &&
        memcmp(left_buf->str, right_buf->str, left_buf->length) == 0;
    strbuf_free(left_buf);
    strbuf_free(right_buf);
    return equal;
}

uint64_t path_hash(Path* path, uint64_t seed0, uint64_t seed1) {
    if (!path) return hashmap_hash_cstr("path:null", seed0, seed1);
    StrBuf* buf = strbuf_new();
    path_to_string(path, buf);
    uint64_t hash = hashmap_hash_bytes(buf->str, buf->length, seed0, seed1);
    strbuf_free(buf);
    return hash;
}

/**
 * Helper to get segment name for OS path output.
 * Returns "*" or "**" for wildcards.
 */
static const char* path_get_os_segment_name(Path* seg_path) {
    LPathSegmentType seg_type = PATH_GET_SEG_TYPE(seg_path);
    switch (seg_type) {
        case LPATH_SEG_WILDCARD: return "*";
        case LPATH_SEG_WILDCARD_REC: return "**";
        case LPATH_SEG_DYNAMIC: return "<dynamic>";
        default: return seg_path->name ? seg_path->name : "";
    }
}

static void path_append_os_component(StrBuf* out, Path* op) {
    LPathSegmentType type = PATH_GET_SEG_TYPE(op);
    if (type == LPATH_SEG_INT) {
        strbuf_append_int64(out, op->int_value);
    } else {
        strbuf_append_str(out, path_get_os_segment_name(op));
    }
}

// A UNC authority must not become a Windows device namespace or another path.
static bool path_unc_authority_valid(const char* host) {
    if (!host || !*host || strcmp(host, ".") == 0 || strcmp(host, "..") == 0) return false;
    for (const unsigned char* p = (const unsigned char*)host; *p; p++) {
        if (*p < 32 || strchr("\\/:*?\"<>|", *p)) return false;
    }
    return true;
}

static bool path_file_authority_local_for_platform(Path* path, bool windows);

bool path_has_nul_name(Path* path) {
    Path* root = path_root_of(path);
    for (Path* step = path; step && step != root; step = step->parent) {
        if (PATH_GET_SEG_TYPE(step) == LPATH_SEG_NORMAL && step->name &&
                memchr(step->name, '\0', step->name_length)) return true;
    }
    return false;
}

/**
 * Convert path to OS file path (e.g., "/etc/hosts" or "C:\Users\name").
 * Handles wildcards properly using segment type flags.
 */
static void path_to_platform_path(Path* path, void* out_ptr, bool windows) {
    StrBuf* out = (StrBuf*)out_ptr;
    if (!path || !out || path_has_nul_name(path)) return;
    bool remote_file = path_get_scheme(path) == PATH_SCHEME_FILE &&
        !path_file_authority_local_for_platform(path, windows);
    if (remote_file && !windows) {
        log_error("path_to_os_path: remote file authority is not supported");
        return;
    }
    if (remote_file && !path_unc_authority_valid(path_root_of(path)->authority_name)) {
        log_error("path_to_os_path: invalid UNC authority");
        return;
    }
    if (path_get_scheme(path) == PATH_SCHEME_LOGICAL) {
        log_error("path_to_os_path: logical root must be qualified first");
        return;
    }

    Path* root = path_root_of(path);
    ArrayList* ops = path_collect_ops(path, root);
    if (!ops) return;
    PathScheme scheme = (PathScheme)root->root_scheme;
    bool skip_drive_component = false;

    if (scheme == PATH_SCHEME_REL) {
        strbuf_append_str(out, "./");
    } else if (scheme == PATH_SCHEME_PARENT) {
        strbuf_append_str(out, "../");
    } else if (scheme == PATH_SCHEME_FILE || scheme == PATH_SCHEME_LOGICAL) {
        if (remote_file) {
            // A Windows remote authority is a UNC server, not a local prefix.
            strbuf_append_str(out, "\\\\");
            strbuf_append_str(out, root->authority_name);
        } else if (windows && ops->length > 0) {
            Path* first = (Path*)arraylist_get(ops, ops->length - 1);
            const char* drive = path_get_os_segment_name(first);
            if (strlen(drive) == 1 && ((drive[0] >= 'A' && drive[0] <= 'Z') ||
                                        (drive[0] >= 'a' && drive[0] <= 'z'))) {
                strbuf_append_char(out, drive[0]);
                strbuf_append_str(out, ":\\");
                skip_drive_component = true;
            } else {
                strbuf_append_char(out, '\\');
            }
        } else if (windows) {
            strbuf_append_char(out, '\\');
        } else {
            strbuf_append_char(out, '/');
        }
    } else {
        strbuf_append_all(out, 2, scheme_names[scheme], "://");
    }

    for (int i = ops->length - 1; i >= 0; i--) {
        if (skip_drive_component && i == ops->length - 1) continue;
        Path* op = (Path*)arraylist_get(ops, i);
        LPathSegmentType type = PATH_GET_SEG_TYPE(op);
        if (type == LPATH_SEG_ROOT) continue;
        if (type == LPATH_SEG_PARENT) {
            strbuf_append_str(out, "../");
            continue;
        }
        if (out->length > 0) {
            char last = out->str[out->length - 1];
            if (last != '/' && last != '\\') strbuf_append_char(out, remote_file ? '\\' : '/');
        }
        path_append_os_component(out, op);
    }
    arraylist_free(ops);
}

void path_to_windows_path(Path* path, void* out) {
    path_to_platform_path(path, out, true);
}

void path_to_os_path(Path* path, void* out) {
#ifdef _WIN32
    path_to_windows_path(path, out);
#else
    path_to_platform_path(path, out, false);
#endif
}

/**
 * Get root path by name (for parser integration).
 */
Path* path_get_root_by_name(const char* name) {
    if (!name) return NULL;

    for (int i = 0; i < PATH_SCHEME_COUNT; i++) {
        if (strcmp(scheme_names[i], name) == 0) {
            return path_get_root((PathScheme)i);
        }
    }

    return NULL;  // unknown scheme
}

/**
 * Build a path segment by segment (internal helper).
 * Returns a new Path with the segment appended.
 * segment_type should be one of LPATH_SEG_NORMAL, LPATH_SEG_WILDCARD, etc.
 */
static Path* path_append_segment_typed(Pool* pool, Path* parent, const char* segment, LPathSegmentType seg_type) {
    if (!parent) {
        log_error("path_append_segment_typed: NULL parent");
        return NULL;
    }
    if (!pool) {
        // Every new segment must use its caller's owning context pool; a
        // missing owner used to dereference NULL after a batch context reset.
        log_error("path_append_segment_typed: NULL pool");
        return NULL;
    }

    return path_alloc_op(pool, parent, seg_type, segment,
                         segment ? strlen(segment) : 0, 0);
}

// ============================================================================
// New Path API: path_new, path_extend, path_wildcard, path_wildcard_recursive
// ============================================================================

/**
 * Create a new path starting with the given scheme.
 * This returns the root path for the scheme.
 */
Path* path_new(Pool* pool, int scheme) {
    if (!pool) {
        log_error("path_new: NULL pool");
        return NULL;
    }
    if (scheme < 0 || scheme >= PATH_SCHEME_COUNT) {
        log_error("path_new: invalid scheme %d", scheme);
        return NULL;
    }
    // Compiled paths receive the owning pool explicitly.  Do not route them
    // through cached process-global roots, which may belong to a prior batch.
    Path* root = (Path*)pool_calloc(pool, sizeof(Path));
    if (!root) return NULL;
    root->type_id = LMD_TYPE_PATH;
    root->name = scheme_names[scheme];
    root->name_length = strlen(root->name);
    root->parent = &ROOT_SENTINEL;
    root->root_scheme = (uint8_t)scheme;
    root->authority_kind = (scheme == PATH_SCHEME_FILE) ? PATH_AUTHORITY_LOCAL : PATH_AUTHORITY_NONE;
    return root;
}

Path* path_clone(Pool* pool, Path* source) {
    if (!pool || !source || source->root_scheme >= PATH_SCHEME_COUNT) return NULL;

    Path* root = source->authority_kind == PATH_AUTHORITY_NAMED && source->authority_name
        ? path_new_authority(pool, source->root_scheme, source->authority_name)
        : path_new(pool, source->root_scheme);
    if (!root) return NULL;
    root->flags |= path_root_of(source)->flags & PATH_FLAG_FILE_SLASH;

    // Replaying the spine gives the destination its own names and deliberately
    // omits resolved content and metadata cached by the source path.
    return path_concat(pool, root, source);
}

/**
 * Extend an existing path with a new normal segment.
 * Returns a new path with the segment appended.
 * The original path is not modified.
 */
Path* path_extend(Pool* pool, Path* base, const char* segment) {
    if (!base) {
        log_error("path_extend: NULL base path");
        return NULL;
    }
    if (!segment) {
        log_error("path_extend: NULL segment");
        return base;
    }
    return path_extend_len(pool, base, segment, strlen(segment));
}

// A shape name is a StrView into a larger buffer and carries no terminator, so
// `&` (which spells a node's path out of the document's node table) needs a
// length-taking extend rather than a strlen one.
Path* path_extend_len(Pool* pool, Path* base, const char* segment, size_t len) {
    if (!base) {
        log_error("path_extend_len: NULL base path");
        return NULL;
    }
    if (!segment) {
        log_error("path_extend_len: NULL segment");
        return base;
    }
    if (!pool) {
        log_error("path_extend_len: NULL pool");
        return NULL;
    }
    return path_alloc_op(pool, base, LPATH_SEG_NORMAL, segment, len, 0);
}

Path* path_extend_int(Pool* pool, Path* base, int64_t value) {
    if (!base || value < 0) return base;
    return path_alloc_op(pool, base, LPATH_SEG_INT, NULL, 0, value);
}

Path* path_select_parent(Pool* pool, Path* base) {
    if (!base) return NULL;
    PathScheme scheme = path_get_scheme(base);
    LPathSegmentType type = PATH_GET_SEG_TYPE(base);

    if (type == LPATH_SEG_PARENT && scheme == PATH_SCHEME_REL) {
        return path_alloc_op(pool, base, LPATH_SEG_PARENT, NULL, 0, 0);
    }
    if (path_is_child(base)) return base->parent;
    if (scheme == PATH_SCHEME_REL) {
        return path_alloc_op(pool, base, LPATH_SEG_PARENT, NULL, 0, 0);
    }
    // Anchored roots clamp at their anchor. This is the closed path algebra
    // required by S2.4.2v3; ordinary values use the dynamic navigation path.
    return base;
}

Path* path_select_root(Pool* pool, Path* base) {
    if (!base) return NULL;
    Path* root = path_root_of(base);
    if (path_get_scheme(base) != PATH_SCHEME_REL) return root;
    if (PATH_GET_SEG_TYPE(base) == LPATH_SEG_ROOT && base->parent == root) return base;
    return path_alloc_op(pool, root, LPATH_SEG_ROOT, NULL, 0, 0);
}

static bool path_file_authority_local_for_platform(Path* path, bool windows) {
    if (!path || path_get_scheme(path) != PATH_SCHEME_FILE) return false;
    Path* root = path_root_of(path);
    if (root->authority_kind != PATH_AUTHORITY_NAMED) return true;
    // S2.4.5v6: every named Windows host addresses its UNC namespace.
    if (windows) return false;
#ifdef LAMBDA_NO_SYSINFO
    // a named file authority cannot be verified without host discovery.
    return false;
#else
    char* current = shell_get_hostname();
    bool local = current && root->authority_name && strcmp(current, root->authority_name) == 0;
    if (current) mem_free(current);
    return local;
#endif
}

bool path_file_authority_is_local(Path* path) {
#ifdef _WIN32
    return path_file_authority_local_for_platform(path, true);
#else
    return path_file_authority_local_for_platform(path, false);
#endif
}

bool path_file_authority_is_available(Path* path) {
#ifdef _WIN32
    // Windows can project a remote file authority as an OS UNC path.
    if (!path || path_get_scheme(path) != PATH_SCHEME_FILE) return false;
    Path* root = path_root_of(path);
    return root->authority_kind != PATH_AUTHORITY_NAMED ||
        path_unc_authority_valid(root->authority_name);
#else
    return path_file_authority_is_local(path);
#endif
}

/**
 * Extend an existing path with another path's segments.
 * Appends all segments from suffix to base.
 * Skips the scheme root of the suffix (only appends actual path segments).
 * Returns a new path.
 */
Path* path_concat(Pool* pool, Path* base, Path* suffix) {
    if (!base) return suffix;
    if (!suffix) return base;

    ArrayList* ops = arraylist_new(8);
    Path* suffix_root = path_root_of(suffix);
    for (Path* p = suffix; p && p != suffix_root; p = p->parent) {
        arraylist_append(ops, p);
    }
    Path* result = base;
    for (int i = ops->length - 1; i >= 0; i--) {
        result = path_append_op(pool, result, (Path*)arraylist_get(ops, i));
    }
    arraylist_free(ops);
    return result;
}

// Apply one operation node of another path to `base`, normalizing parent and
// root steps as S2.4.2v5 requires.
Path* path_append_op(Pool* pool, Path* base, Path* op) {
    LPathSegmentType type = PATH_GET_SEG_TYPE(op);
    if (type == LPATH_SEG_QUERY) {
        return path_query(pool, base, op->query_type, (op->flags & PATH_FLAG_QUERY_SELF) != 0);
    }
    if (type == LPATH_SEG_PARENT) return path_select_parent(pool, base);
    if (type == LPATH_SEG_ROOT) return path_select_root(pool, base);
    if (type == LPATH_SEG_INT) return path_extend_int(pool, base, op->int_value);
    if (type == LPATH_SEG_NORMAL) return path_alloc_op(pool, base, type, op->name, op->name_length, 0);
    return path_append_segment_typed(pool, base, op->name, type);
}

// Qualify the logical resolver root through the default local-file mount.
// Qualification returns a new spine and leaves the source reference intact.
Path* path_qualify_default(Pool* pool, Path* path) {
    if (!pool || !path) return path;
    if (path_get_scheme(path) != PATH_SCHEME_LOGICAL) return path;
    return path_concat(pool, path_new(pool, PATH_SCHEME_FILE), path);
}

/**
 * Create a wildcard segment (*) - matches any single path component.
 * Uses LPATH_SEG_WILDCARD flag instead of storing "*" as string.
 */
Path* path_wildcard(Pool* pool, Path* base) {
    if (!base) {
        log_error("path_wildcard: NULL base path");
        return NULL;
    }
    return path_append_segment_typed(pool, base, NULL, LPATH_SEG_WILDCARD);
}

/**
 * Create a recursive wildcard segment (**) - matches zero or more path components.
 * Uses LPATH_SEG_WILDCARD_REC flag instead of storing "**" as string.
 */
Path* path_wildcard_recursive(Pool* pool, Path* base) {
    if (!base) {
        log_error("path_wildcard_recursive: NULL base path");
        return NULL;
    }
    return path_append_segment_typed(pool, base, NULL, LPATH_SEG_WILDCARD_REC);
}

/**
 * Check if a path segment is a single wildcard (*).
 * Uses the segment type flag, not string comparison.
 */
bool path_is_wildcard(Path* path) {
    if (!path) return false;
    return PATH_GET_SEG_TYPE(path) == LPATH_SEG_WILDCARD;
}

/**
 * Check if a path segment is a recursive wildcard (**).
 * Uses the segment type flag, not string comparison.
 */
bool path_is_wildcard_recursive(Path* path) {
    if (!path) return false;
    return PATH_GET_SEG_TYPE(path) == LPATH_SEG_WILDCARD_REC;
}

Path* path_query(Pool* pool, Path* base, uint64_t type_item, bool self) {
    if (!base || !type_item) return NULL;
    Path* path = path_alloc_op(pool, base, LPATH_SEG_QUERY, NULL, 0, 0);
    if (!path) return NULL;
    path->query_type = type_item;
    if (self) path->flags |= PATH_FLAG_QUERY_SELF;
    return path;
}

/**
 * Check if a path contains any enumerating step: a wildcard (S2.4.6) or a
 * type query (S2.4.7). Forcing such a path continues per match.
 */
bool path_has_wildcards(Path* path) {
    Path* p = path;
    while (p && p->parent) {
        LPathSegmentType seg_type = PATH_GET_SEG_TYPE(p);
        if (seg_type == LPATH_SEG_WILDCARD || seg_type == LPATH_SEG_WILDCARD_REC ||
                seg_type == LPATH_SEG_QUERY) {
            return true;
        }
        p = p->parent;
    }
    return false;
}

// ============================================================================
// Path to string conversion - updated for segment types
// ============================================================================

// A Path carries a captured metadata value; acquisition stays in the provider.
bool path_apply_stat_metadata(Path* path, Pool* pool, const FileStat* fs) {
    if (!path || !pool || !fs) return false;
    PathMeta* meta = NULL;
    if (fs->exists) {
        meta = (PathMeta*)pool_calloc(pool, sizeof(PathMeta));
        DateTime* modified = datetime_from_unix(pool, (int64_t)fs->modified);
        if (!meta || !modified) return false;
        meta->size = fs->size;
        meta->modified = *modified;
        if (fs->is_dir) meta->flags |= PATH_META_IS_DIR;
        if (fs->is_symlink) meta->flags |= PATH_META_IS_LINK;
        meta->mode = (fs->mode >> 6) & 0x07;
    }
    path->meta = meta;
    path->flags |= PATH_FLAG_META_LOADED;
    return true;
}
