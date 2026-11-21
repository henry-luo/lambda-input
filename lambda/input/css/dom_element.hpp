#ifndef DOM_ELEMENT_H
#define DOM_ELEMENT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "lib/avl_tree.h"
#include "lib/arraylist.h"
#include "lib/arena.h"
#include "lib/ownership.hpp"
#include "lib/strbuf.h"
#include "css_style.hpp"
#include "css_style_node.hpp"
#include "dom_lifecycle.hpp"
#include "dom_node.hpp"  // Provides DomNodeType enum and utility functions
#include "html_input.hpp"
#include "../../core/value.hpp"  // Full Element definition (needed for embedded Element field)
#include "../../core/name_identity.h"
#include "../../io/resource_policy.h"

/**
 * DOM Element Extension for CSS Styling
 *
 * This module extends Lambda's Element structure to support AVL tree-based
 * CSS style management. It provides efficient style resolution, cascade
 * computation, and caching for high-performance rendering.
 *
 * Integration with Radiant:
 * - Uses C++ inheritance for polymorphic node operations
 * - Provides CSS style lookup through AVL trees (O(log n))
 * - Caches computed values for performance
 * - Supports dynamic style updates
 *
 * Note: DomNode is the base class for DomElement, DomText, and DomComment,
 * providing polymorphic DOM tree operations.
 */

// Forward declarations
typedef struct Element Element;
typedef struct Input Input;
typedef struct Arena Arena;
typedef struct ViewTree ViewTree;  // From radiant/view.hpp
typedef struct LayoutContext LayoutContext;  // From radiant/layout.hpp
typedef struct DocState DocState;  // From radiant/state_store.h
typedef struct StateStore StateStore;  // From radiant/state_store.hpp
typedef struct Url Url;  // From lib/url.h
struct DomScriptReaction;

// Node-level style and presentation invalidation is shared by both hosts.
void dom_mark_dirty_ancestors(DomNode* node);
void dom_layer_note_mutation(DomNode* node, bool moves_only = false);
extern "C" void dom_invalidate_layout_subtree(void* node);
extern "C" void dom_svg_invalidate_presentation(void* node);
extern "C" uint64_t dom_mutation_epoch(DomDocument* doc);

// shared slot assignment for the DOM, HTML directionality and the renderer.
struct DomSlotAssignment {
    DomElement* slot;
    DomElement* host;
    bool manual;
};
struct DomManualSlotInput {
    DomDocument* document;
    DomNode* node;
};
DomElement* dom_shadow_root(DomNode* node);
DomElement* dom_shadow_first_matching_slot(DomNode* root, const char* name);
DomSlotAssignment dom_slot_assignment(DomElement* slot);
DomNode* dom_slot_assigned_adjacent(const DomSlotAssignment& assignment,
                                  DomNode* current = nullptr, bool forward = true);
bool dom_slot_set_manual_assignment(DomElement* slot, const DomManualSlotInput* inputs,
                                   int count, ArrayList* affected_roots);
void dom_slot_release_manual_links(DomNode* node, DomManualSlotLink* binding);
void dom_slot_transfer_manual_link(DomManualSlotLink** source, DomManualSlotLink** destination);
DomElement* dom_find_assigned_slot(DomNode* node);
bool dom_node_is_slottable(DomNode* node);
DomElement* dom_flat_tree_parent(DomElement* element);
bool dom_node_is_inert(DomNode* node);
bool dom_top_layer_add(DomElement* element, DomElement* anchor = nullptr);
void dom_top_layer_remove(DomElement* element);
void dom_top_layer_rebind(DomElement* previous, DomElement* replacement);
size_t dom_top_layer_count(const DomDocument* document);
DomElement* dom_top_layer_at(DomDocument* document, size_t index);
DomElement* dom_top_modal_dialog(DomDocument* document);

typedef const char* (*DomDirectionValueResolver)(DomElement*, void*);
// Shared HTML first-strong scan used by dir=auto layout and :dir() matching.
int dom_find_strong_direction(DomNode* node, bool skip_explicit_dir, bool first,
    bool raw_content = false, DomDirectionValueResolver resolve_value = nullptr,
    void* context = nullptr);
int dom_element_directionality(DomElement* element,
    DomDirectionValueResolver resolve_value = nullptr, void* context = nullptr);
bool dom_element_has_directionality_hint(DomElement* element);
CssEnum dom_html_direction_state(DomElement* element);
extern "C" int dom_css_element_directionality(void* element);
typedef struct VectorPathProp VectorPathProp;  // From radiant/view.hpp
typedef struct MultiColumnProp MultiColumnProp;  // From radiant/view.hpp
typedef struct MarkerProp MarkerProp;  // From radiant/view.hpp
typedef struct CssTransitionTrack CssTransitionTrack;  // From radiant/view.hpp
// D5.5.1: DOM retirement owns the snapshot and its track allocation.
typedef struct CssTransitionElemState {
    lam::OwnArr<CssTransitionTrack> tracks;
    int track_count;
    int track_capacity;
    Pool* pool;
} CssTransitionElemState;
typedef struct CssWebAnimationState CssWebAnimationState;  // From radiant/view.hpp
typedef struct CustomLayoutPaintState CustomLayoutPaintState;  // From radiant/layout.hpp
typedef struct Runtime Runtime;  // From lambda/lambda.h
struct DomElement;
extern "C" DomElement* dom_find_element_by_id(DomElement* root, const char* id);
bool dom_svg_tag_is(DomElement* element, const char* tag);
const char* dom_element_namespace_uri(struct DomElement* element);
// namespace predicates are shared data queries for layout and the public DOM adapter.
bool dom_element_is_svg(struct DomElement* element);
NameId dom_element_html_tag(struct DomElement* element);
// same as dom_element_html_tag(element) == tag, without the namespace walk for other tags
bool dom_element_is_html_tag(struct DomElement* element, NameId tag);
bool dom_custom_element_name_is_valid(StrView name);
bool dom_html_shadow_host_name(const char* name);

struct DomShadowInit {
    bool closed;
    bool delegates_focus;
    bool manual_slot_assignment;
    bool clonable;
    bool serializable;
    bool declarative;
};
DomElement* dom_attach_shadow_root(DomElement* host, DomShadowInit options);
const char* dom_element_lookup_namespace_uri(struct DomElement* element, const char* prefix);
const char* dom_element_attribute_namespace_uri(struct DomElement* element,
    const char* qualified_name, const char** local_name);

// ============================================================================
// DOM Document
// ============================================================================

typedef enum HtmlVersion {
    HTML5 = 1,
    HTML4_01_STRICT,
    HTML4_01_TRANSITIONAL,
    HTML4_01_FRAMESET,
    HTML_QUIRKS,
    HTML1_0,
    HTML_LIMITED_QUIRKS,
} HtmlVersion;

inline bool is_quirks_mode(HtmlVersion version) {
    return version == HTML4_01_TRANSITIONAL ||
        version == HTML4_01_FRAMESET || version == HTML_QUIRKS ||
        version == HTML1_0;
}

inline bool is_limited_quirks_mode(HtmlVersion version) {
    return version == HTML_LIMITED_QUIRKS;
}
bool dom_element_uses_quirks_css(const DomElement* element);

typedef enum DomJsMutationKind {
    DOM_JS_MUTATION_UNKNOWN = 0,
    DOM_JS_MUTATION_CHILD_INSERT = 1,
    DOM_JS_MUTATION_CHILD_REMOVE = 2,
    DOM_JS_MUTATION_TEXT = 3,
    DOM_JS_MUTATION_ATTRIBUTE = 4,
    DOM_JS_MUTATION_STYLE = 5,
    DOM_JS_MUTATION_TREE_REPLACE = 6,
    DOM_JS_MUTATION_STYLE_REPAINT = 7,
    DOM_JS_MUTATION_CONTROL_VALUE = 8,
    // A direct element.style/style-attribute write already updates the
    // element's inline declarations and cannot change selector matching.
    DOM_JS_MUTATION_INLINE_STYLE = 9,
    DOM_JS_MUTATION_SLOT_DISTRIBUTION = 10,
} DomJsMutationKind;

// authored and presentation writes share repaint versus recascade classification.
inline DomJsMutationKind dom_style_mutation_kind(CssPropertyCode prop_id) {
    switch (prop_id) {
        case CSS_PROPERTY_BACKGROUND_COLOR:
        case CSS_PROPERTY_COLOR:
        case CSS_PROPERTY_OPACITY:
        case CSS_PROPERTY_VISIBILITY:
            return DOM_JS_MUTATION_STYLE_REPAINT;
        default:
            return DOM_JS_MUTATION_INLINE_STYLE;
    }
}

typedef enum DomJsMutationAttribute {
    DOM_JS_MUTATION_ATTRIBUTE_UNKNOWN,
    DOM_JS_MUTATION_ATTRIBUTE_CLASS,
    // a data/ARIA attribute absent from relational selector dependencies
    DOM_JS_MUTATION_ATTRIBUTE_LOCAL,
    DOM_JS_MUTATION_ATTRIBUTE_SLOT_DISTRIBUTION,
} DomJsMutationAttribute;

// tier-1: document-owned journal, survives relayout
typedef struct DomJsMutationRecord {
    uint32_t sequence;
    DomJsMutationKind kind;
    DomNode* target;
    DomNode* parent;
    uint32_t target_id;
    uint32_t parent_id;
    DomJsMutationAttribute attribute;
    // The property a presentation-channel write changed (CSS_PROPERTY_UNKNOWN
    // otherwise); such writes leave authored attributes and selectors intact.
    CssPropertyCode presentation_property;
    // Capture connection at mutation time; a later append must not make a
    // detached node's earlier attribute/text writes look document-visible.
    bool was_connected;
    // captured before removal/reparenting changes first-strong ancestry.
    bool direction_sensitive;
} DomJsMutationRecord;

#define DOM_JS_MUTATION_RECORD_CAP 64

typedef enum DomReconcileMode {
    DOM_RECONCILE_NONE = 0,
    DOM_RECONCILE_INCREMENTAL = 1,
    DOM_RECONCILE_FULL = 2,
    DOM_RECONCILE_RETAINED_FULL_LAYOUT = 3,
    DOM_RECONCILE_DESTRUCTIVE_REBUILD = 4,
    DOM_RECONCILE_DOCUMENT_REBUILD = 5
} DomReconcileMode;

// tier-1: document-owned runtime state
struct DomJsRuntime {
    void* mir_ctx;
    void* preamble_state;
    // The document owns a complete Runtime, not a copied heap fragment.
    // Timers, DOM callbacks, and JIT entries all borrow its one EvalContext.
    Runtime* runtime;
    void* doc_node;
    bool implicit_doctype;
    bool selected_content_initialized;
    int mutation_count;
    uint32_t mutation_sequence;
    uint32_t mutation_kind_mask;
    int mutation_record_count;
    int mutation_record_overflow;
    // a reflow was already requested when this batch recorded its first mutation
    bool reflow_pending_before_batch;
    // the document was already dirty, and how many full repaints had been requested,
    // when this batch recorded its first mutation (a paint-only commit withdraws only
    // the dirtiness its own reflow requests caused)
    bool dirty_before_batch;
    uint64_t full_repaint_requests_before_batch;
    DomJsMutationRecord* mutation_records;
    int mutation_record_capacity;
    DomJsMutationRecord inline_mutation_records[DOM_JS_MUTATION_RECORD_CAP];
    // DOM text-tree edits to <style> need exact owners even when the generic
    // layout-mutation ledger cannot grow after an allocation failure.
    DomElement** inline_stylesheet_mutations;
    int inline_stylesheet_mutation_count;
    int inline_stylesheet_mutation_capacity;
    const char* ready_state;
    // Host setup is copied into this document before its Runtime exists.  The
    // script runner transfers it into the bound EvalContext before any JS can
    // schedule timers or read DOM geometry.
    void* host_ui_context;
    bool host_driven_loop;
    bool auto_close_event_loop;
    bool virtual_clock_enabled;
    double virtual_clock_ms;
    double post_load_settle_ms;
    bool redirect_stdout_to_stderr;
    // Set only while a classic script is evaluated, matching HTML's
    // document.currentScript lifetime.
    DomElement* current_script;
    DomNode* write_cursor;
    void (*write_script_subtree)(void* context, DomElement* element);
    void* write_script_context; // borrowed only during the loader's script scheduler.
    void (*prepare_inline_script)(DomElement* element, StrBuf** source_owner);
    void* preparing_script_preamble; // borrowed during the initial script drain.
    DomScriptReaction* pending_scripts;
    DomScriptReaction* pending_scripts_tail;
    DomScriptReaction* running_scripts;
    uint32_t script_reaction_depth;
    bool has_shadow_roots;

    DomJsRuntime() : mir_ctx(nullptr), preamble_state(nullptr), runtime(nullptr),
        doc_node(nullptr), implicit_doctype(true), mutation_count(0), mutation_sequence(0), mutation_kind_mask(0),
        mutation_record_count(0), mutation_record_overflow(0),
        mutation_records(inline_mutation_records), mutation_record_capacity(DOM_JS_MUTATION_RECORD_CAP),
        inline_mutation_records{},
        inline_stylesheet_mutations(nullptr), inline_stylesheet_mutation_count(0),
        inline_stylesheet_mutation_capacity(0),
        ready_state("complete"), host_ui_context(nullptr), host_driven_loop(false),
        auto_close_event_loop(false),
        virtual_clock_enabled(false), virtual_clock_ms(0.0),
        post_load_settle_ms(0.0),
        redirect_stdout_to_stderr(false), current_script(nullptr), write_cursor(nullptr),
        write_script_subtree(nullptr), write_script_context(nullptr), prepare_inline_script(nullptr),
        preparing_script_preamble(nullptr), pending_scripts(nullptr), pending_scripts_tail(nullptr),
        running_scripts(nullptr), script_reaction_depth(0), has_shadow_roots(false) {}
};

// tier-1: document-owned semantic zoom and raster-output scale inputs
struct ViewportMeta {
    float output_scale;
    float raster_scale;
    float page_zoom;
    float min_scale;
    float max_scale;
    int width;
    int height;
    float body_transform_scale;

    ViewportMeta() : output_scale(1.0f), raster_scale(1.0f), page_zoom(1.0f),
        min_scale(0.0f), max_scale(0.0f), width(0), height(0),
        body_transform_scale(1.0f) {}
};

static_assert(sizeof(ViewportMeta) == 32,
              "ViewportMeta is embedded before DomDocument ABI-sensitive members");

// tier-1: last reconciliation result exposed to diagnostics and tests
struct ReconcileLog {
    DomReconcileMode mode;
    lam::Up<const char> reason;  // a static description
    int mutations;
    int records;
    int record_overflow;

    ReconcileLog() : mode(DOM_RECONCILE_NONE), reason("none"), mutations(0),
        records(0), record_overflow(0) {}
};

struct DomSharedPseudoResets;
struct CssSelector;
enum DomPopoverMode : uint8_t { DOM_POPOVER_NONE, DOM_POPOVER_AUTO, DOM_POPOVER_HINT, DOM_POPOVER_MANUAL };

// tier-1: opaque services kept out of the public document spine
struct DomDocumentServices {
    void* mem_ctx;
    void* cached_css_engine;
    lam::Up<CssStylesheet> ua_stylesheet; // document-pool UA rules, excluded from CSSOM styleSheets
    void* keyframe_registry;
    void* registered_property_set; // document-owned CSS.registerProperty definitions
    uint32_t element_count;
    uint32_t ext_allocations;
    uint32_t layout_cache_allocations;
    void* node_registry;       // external generations, pins, and detached candidates
    void* style_epoch_manager; // versioned canonical specified-style pools
    void* scene3d_registry;   // document-owned native 3D viewport projections
    void* canvas_registry;     // document-owned HTMLCanvasElement backing surfaces
    void* svg_geometry_cache; // document-owned immutable SVG path resources
    void* svg_layer_registry;  // document-owned inline <svg> raster layers (render_svg_inline.cpp)
    void* paint_layer_registry;  // document-owned HTML paint layers (render_paint_layer.cpp)
    void* shadow_image_cache;  // document-owned transformed box-shadow rasters (render_background.cpp)
    void* svg_filter_registry; // document-owned compiled SVG filter programs
    void* svg_animation_registry; // document-owned SMIL clocks and sampled values
    void* svg_use_resource_cache; // retained external-use DOM/style owners
    char* preferred_languages; // document-owned UI preference snapshot, refreshed by the host setter
    lam::Own<char> pragma_language;    // document-owned default retained after a meta insertion
    lam::Own<char> protocol_language;  // document-owned single Content-Language response value
    lam::Up<const char> html_namespace_uri, empty_namespace_uri; // Input name-pool bytes
    lam::Own<struct hashmap> tag_name_overrides = nullptr; // spellings absent from the backing type and catalog
    lam::Own<DomSharedPseudoResets> shared_pseudo_resets = nullptr; // physical document pool
    bool svg_source_document = false; // standalone SVG source uses DOM serialization on SVG save
    bool svg_image_document; // SVG image processing forbids external subordinate resources
    void* popover_state; // document-owned Lambda visibility stacks
    struct DomTopLayer* top_layer = nullptr; // document-owned order shared by HTML and paint

    DomDocumentServices() : mem_ctx(nullptr), cached_css_engine(nullptr), ua_stylesheet(nullptr),
        keyframe_registry(nullptr), registered_property_set(nullptr), element_count(0), ext_allocations(0),
        layout_cache_allocations(0), node_registry(nullptr),
        style_epoch_manager(nullptr), scene3d_registry(nullptr), canvas_registry(nullptr),
        svg_geometry_cache(nullptr), svg_layer_registry(nullptr), paint_layer_registry(nullptr), shadow_image_cache(nullptr), svg_filter_registry(nullptr), svg_animation_registry(nullptr), svg_use_resource_cache(nullptr), preferred_languages(nullptr), pragma_language(nullptr), protocol_language(nullptr),
        html_namespace_uri(nullptr), empty_namespace_uri(nullptr), svg_image_document(false), popover_state(nullptr) {}
};

static inline const char* dom_reconcile_mode_name(DomReconcileMode mode) {
    switch (mode) {
        case DOM_RECONCILE_INCREMENTAL: return "incremental";
        case DOM_RECONCILE_FULL: return "full";
        case DOM_RECONCILE_RETAINED_FULL_LAYOUT: return "retained_full_layout";
        case DOM_RECONCILE_DESTRUCTIVE_REBUILD: return "destructive_rebuild";
        case DOM_RECONCILE_DOCUMENT_REBUILD: return "document_rebuild";
        case DOM_RECONCILE_NONE:
        default: return "none";
    }
}

// Document provenance: what the author actually wrote. Routing and script-realm
// decisions must consult this (and js_has_dom_realm) rather than inferring page
// nature from which runtime pointer happens to be set — a document may legally
// host both page JS and Lambda code.
typedef enum DomPageKind {
    DOM_PAGE_KIND_UNKNOWN = 0,     // not yet classified by a loader
    DOM_PAGE_KIND_HTML,            // parsed HTML/JS page; may host a JS DOM realm
    DOM_PAGE_KIND_LAMBDA_SCRIPT,   // .ls page whose script produced the tree
    DOM_PAGE_KIND_GENERATED        // engine-generated tree (markdown, XML, LaTeX, bridges)
} DomPageKind;

static inline const char* dom_page_kind_name(DomPageKind kind) {
    switch (kind) {
        case DOM_PAGE_KIND_HTML: return "html";
        case DOM_PAGE_KIND_LAMBDA_SCRIPT: return "lambda-script";
        case DOM_PAGE_KIND_GENERATED: return "generated";
        case DOM_PAGE_KIND_UNKNOWN:
        default: return "unknown";
    }
}

/**
 * DomDocument - Root container for DOM tree
 * Manages memory (arena) and Lambda integration (Input*)
 * Unified document structure (replaces radiant/dom.hpp Document)
 */
// tier-1: doc-pool, survives relayout
enum DomScrollAlign : uint8_t {
    DOM_SCROLL_ALIGN_START,
    DOM_SCROLL_ALIGN_CENTER,
    DOM_SCROLL_ALIGN_END,
    DOM_SCROLL_ALIGN_NEAREST,
};

enum DomScrollBehavior : uint8_t {
    DOM_SCROLL_BEHAVIOR_AUTO,
    DOM_SCROLL_BEHAVIOR_SMOOTH,
    DOM_SCROLL_BEHAVIOR_INSTANT,
};

// Every payload in a document's resource table (registries, caches, adopted
// documents) derives from this; its destroy callback casts down to its type.
struct DomDocumentResourceData {};

struct DomDocument : DomDocumentResourceData {
    // Lambda integration
    Input* input;                // Lambda Input context for MarkEditor operations
    lam::Own<Pool> document_pool;   // Document-owned selectively released objects
    lam::Own<Arena> node_arena;     // Stable DOM nodes and registered node-owned payloads
    DomDocumentServices services;

    // Document content
    lam::Own<Url> url;           // Document URL
    lam::Up<Element> html_root;     // Parsed HTML tree in Mark notation (Lambda tree)
    lam::Up<DomElement> root;       // Root element of DOM tree (optional); owned by the node chain
    int html_version;            // Detected HTML version - maps to HtmlVersion enum
    bool html_scripting_enabled; // HTML parser scripting mode for special elements
    // No script of this document runs: <script>, on* attributes, Lambda
    // documents, <webview>. Set by a sandboxed frame (MQ11), inherited by
    // every document nested in it, and never cleared.
    bool scripting_disabled;
    bool xml_document;          // document factories preserve case in XML documents.
    uint32_t next_node_id;        // next DomNode id for event/state logs (0 reserved)

    // CSS stylesheets (for @font-face processing after UiContext init)
    lam::OwnArr<struct CssStylesheet*> stylesheets;  // Array of parsed stylesheets
    int stylesheet_count;                // Number of stylesheets
    int stylesheet_capacity;             // Capacity of stylesheet array
    bool font_faces_processed;            // @font-face registry matches stylesheets

    // Layout and state
    lam::Own<ViewTree> view_tree;   // View tree after layout
    lam::Own<ViewTree> secondary_view_trees; // Independent layouts over this source DOM.
    bool secondary_views_cleanup_registered;
    lam::Own<StateStore> state_store;  // Per-document state store owner
    lam::Up<DocState> state;        // Compatibility pointer to state_store->doc_state

    ViewportMeta viewport;

    // Network support (Phase 4 integration)
    lam::Own<struct NetworkResourceManager> resource_manager;  // Network resource coordinator (nullptr for local-only docs)
    InputResourcePolicy resource_policy;              // immutable dependency admission for this document
    double load_start_time;                           // Document load start timestamp (for total timeout)
    bool fully_loaded;                                // True when all network resources complete

    // Reactive UI: retained Lambda runtime for event handler execution
    Runtime* lambda_runtime;     // Retained runtime (heap, JIT context) for reactive UI sessions

    // An embedded document has one bounded upward browsing-context edge. The
    // owner reference is generation-checked so package traversal cannot keep a
    // detached iframe alive or follow a recycled node (ES31, D5.5.1).
    lam::Up<DomDocument> embedding_document;
    DomNodeRef embedding_element_ref;
    // The focused browsing context below this document: its iframe element that
    // the last pointer press landed in (empty when the press was in this document
    // itself). Keys with no focused element follow this chain, as in browsers.
    DomNodeRef active_frame_ref;
    // Set on the parent when a document is embedded; its next behavior-init
    // phase gives script-less children their package evaluator (S12.1.3).
    bool embedded_evaluator_pending;

    // Native extensions retain runtime-backed values through document resources.
    lam::Own<struct DomDocumentResource> resources;
    bool destroying; // native teardown has started; no new resource owner may attach

    // Reactive UI: cached CSS for rebuild_lambda_doc optimization
    lam::OwnArr<struct CssStylesheet*> cached_inline_sheets;  // Parsed inline <style> stylesheets (cached)
    int cached_inline_sheet_count;                // Number of cached inline stylesheets

    // Reactive UI: Element* → DomElement* map for incremental DOM rebuild
    lam::Own<struct hashmap> element_dom_map;     // maps Lambda Element* to its DomElement wrapper

    // Phase 15: Skip blanket styles_resolved reset during incremental layout
    bool skip_style_reset;

    // Phase 16: Incremental layout mode — skip pool recreate, skip clean subtrees
    bool incremental_layout;

    // Headless snapshot mode freezes CSS animation effects before layout.
    bool disable_css_animations;

    DomJsRuntime js;

    // Last DOM reconcile result. Tests assert this instead of parsing log.txt,
    // so fallback/state-retention coverage can distinguish broad reflow from
    // destructive document rebuild.
    ReconcileLog reconcile;

    // JS/meta requested document navigation. The loader follows this after
    // load-time scripts and refresh metadata have been processed.
    char* pending_navigation_url;

    // Document charset (from <meta charset> or HTTP Content-Type), for CSS fallback encoding
    lam::Own<const char> document_charset;  // document-pool copy, e.g. "windows-1251"; nullptr means UTF-8

    // JS-requested viewport scroll offsets. Captured after script execution and
    // applied to the root viewport scroller after layout establishes scroll ranges.
    float pending_viewport_scroll_x;
    float pending_viewport_scroll_y;
    DomNodeRef pending_scroll_into_view_target;  // pinned element; the id guards a recycled address
    bool pending_scroll_into_view_center;

    // Keep new editing state at the document tail: native/JIT consumers depend
    // on the offsets of the long-lived DOM/JS members above.
    uint64_t mutation_epoch;

    // Document provenance, set by the loader that built this tree. Kept here at
    // the tail (rather than beside html_version) so no existing member offset
    // shifts for the native/JIT consumers noted above.
    DomPageKind page_kind;
    bool html_scripts_deferred; // Async parse handed script execution to the host thread.

    // True once the script runner has established a real JS DOM script realm on
    // this document. A Lambda-script page may retain a Jube support capsule in
    // `js` without owning a script realm, so realm presence needs its own bit
    // and must never be inferred from lambda_runtime being null.
    bool js_has_dom_realm;

    // A one-shot render has executed page JS but intentionally released its
    // realm. It must not later create a second evaluator for UA behavior.
    bool js_realm_released_after_load;

    // The Lambda dom package supplies this document's UA behavior templates. It
    // loads at most once, on the first event, so static layout and render runs
    // never pay for it.
    bool dom_package_loaded;

    // True when this document created its own script runtime because it had
    // none — a script-less HTML page owns no runtime until UA behavior needs
    // one. The document then owns that runtime's teardown (ESO25).
    bool owns_script_runtime;

    // F8/ES19: at least one form control in this document may not have had its
    // behavior `init` turn yet. Set when a control's prop is created during
    // layout and cleared by the init phase.
    bool behavior_init_pending;

    // designMode is document editing state, not a property of whichever JS
    // realm happens to be active when native input resolves its host (D10.2.1).
    bool design_mode;

    // The legacy document.domain override relaxes same-origin checks without
    // mutating the document URL's hostname.
    lam::Own<const char> document_domain;

    // Loader-created source pools survive the DOM/JS teardown and are released
    // immediately after it. Caller-supplied pools remain external.
    lam::Own<Pool> owned_loader_pool;

    // Keep this queue at the tail with the other non-JIT document state.
    // Layout records live controls here so init need not rescan the DOM.
    lam::Own<ArrayList> behavior_init_controls;

    // Foreign documents can borrow their creator's Input. Only the document
    // assigned a fresh Input may release that Input's document context.
    bool owns_input_resources;
    bool pending_scroll_into_view_if_needed;
    DomScrollAlign pending_scroll_into_view_block;
    DomScrollAlign pending_scroll_into_view_inline;
    DomScrollBehavior pending_scroll_into_view_behavior;

    // The window's shared loader runtime when a stateless loader built this
    // document on it. The UiContext owns that runtime and releases it after
    // its documents; the document borrows it for its package types and its
    // custom layouts. Its UA behavior still gets its own evaluator.
    lam::Up<Runtime> loader_runtime;

    // Advances with mutation_epoch except for presentation writes of
    // non-inherited properties, which reach neither other elements' computed
    // style nor selector matching. Content caches outside the written subtree
    // (inline SVG layers) key on this instead of every DOM write.
    uint64_t style_content_epoch;
    // native attribute/style setters also invalidate computed queries before a DOM commit.
    uint64_t style_query_epoch;

    // Constructor
    DomDocument() : input(nullptr), document_pool(nullptr), node_arena(nullptr),
                    url(nullptr), html_root(nullptr), root(nullptr), html_version(0),
                    html_scripting_enabled(false), scripting_disabled(false), xml_document(false),
                    next_node_id(1),
                    stylesheets(nullptr), stylesheet_count(0), stylesheet_capacity(0),
                    font_faces_processed(false),
                    view_tree(nullptr), secondary_view_trees(nullptr),
                    secondary_views_cleanup_registered(false), state_store(nullptr), state(nullptr),
                    resource_manager(nullptr), resource_policy(INPUT_RESOURCE_ALLOW_NETWORK), load_start_time(0.0), fully_loaded(true),
                    lambda_runtime(nullptr), embedding_document(nullptr),
                    embedding_element_ref({nullptr, 0}), resources(nullptr),
                    cached_inline_sheets(nullptr), cached_inline_sheet_count(0),
                    element_dom_map(nullptr),
                    skip_style_reset(false),
                    incremental_layout(false),
                    disable_css_animations(false),
                    pending_navigation_url(nullptr),
                    document_charset(nullptr),
                    pending_viewport_scroll_x(0.0f), pending_viewport_scroll_y(0.0f),
                    pending_scroll_into_view_target{},
                    pending_scroll_into_view_center(false),
                    mutation_epoch(0), page_kind(DOM_PAGE_KIND_UNKNOWN), js_has_dom_realm(false),
                    js_realm_released_after_load(false), dom_package_loaded(false),
                    owns_script_runtime(false), behavior_init_pending(false),
                    design_mode(false), document_domain(nullptr), owned_loader_pool(nullptr),
                    behavior_init_controls(nullptr), owns_input_resources(false),
                    pending_scroll_into_view_if_needed(false),
                    pending_scroll_into_view_block(DOM_SCROLL_ALIGN_START),
                    pending_scroll_into_view_inline(DOM_SCROLL_ALIGN_NEAREST),
                    pending_scroll_into_view_behavior(DOM_SCROLL_BEHAVIOR_AUTO),
                    loader_runtime(nullptr), style_content_epoch(0), style_query_epoch(0) {}

    bool init(Input* input);
    void destroy();
};

// The one script runtime a document owns. JS and Lambda share it: `js.runtime`
// is itself a Lambda Runtime (shared lambda-rt layer), and one document may not
// multiplex independent evaluators on a host thread, so both realms resolve to
// the same Runtime/EvalContext through this accessor.
static inline Runtime* dom_document_script_runtime(const DomDocument* doc) {
    if (!doc) return nullptr;
    return doc->lambda_runtime ? doc->lambda_runtime : doc->js.runtime;
}

// The runtime whose packages built this document and serve its custom
// layouts: the window's shared loader runtime when one built it, else the
// document's own runtime.
static inline Runtime* dom_document_loader_runtime(const DomDocument* doc) {
    if (!doc) return nullptr;
    return doc->loader_runtime ? doc->loader_runtime.get() : dom_document_script_runtime(doc);
}

// Does this document own a live JS DOM script realm? Capability, not provenance:
// a page may carry Lambda code and a JS realm at once, so callers must test this
// instead of `!doc->lambda_runtime`.
static inline bool dom_document_has_js_realm(const DomDocument* doc) {
    return doc && doc->js_has_dom_realm && doc->js.runtime;
}

uint32_t dom_document_alloc_node_id(DomDocument* doc);

// A DOM node can be owned either by the document's dedicated node arena or by
// the retained Input arena when UI-mode values embed their DOM storage.
bool dom_document_owns_node_storage(DomDocument* doc, const void* storage);

typedef void (*DomDocumentResourceDestroyFn)(struct DomDocumentResourceData* data);

// tier-1: doc-pool, survives relayout
typedef struct DomDocumentResource {
    lam::Own<struct DomDocumentResourceData> data;   // released by `destroy`
    DomDocumentResourceDestroyFn destroy;
    lam::Up<Context> runtime_owner; // release before this evaluator's context retires (D6.3.3).
    DomDocumentResourceDestroyFn retire_runtime; // optional: preserve native state after unrooting.
    lam::Own<DomDocumentResource> next;
    bool (*pending_work)(DomDocumentResourceData* data); // native jobs awaiting execution/publication.
} DomDocumentResource;

bool dom_document_add_resource(DomDocument* document, struct DomDocumentResourceData* data,
                               DomDocumentResourceDestroyFn destroy, Context* runtime_owner = nullptr,
                               DomDocumentResourceDestroyFn retire_runtime = nullptr);
bool dom_document_release_resource(DomDocument* document, DomDocumentResourceData* data);
bool dom_document_set_resource_pending_work(DomDocument* document, DomDocumentResourceData* data,
    bool (*pending)(DomDocumentResourceData*));
bool dom_document_has_pending_work(DomDocument* document);
bool dom_document_bind_resource_runtime(DomDocument* document, DomDocumentResourceData* data,
    Context* runtime_owner, DomDocumentResourceDestroyFn retire_runtime = nullptr);
void dom_document_release_runtime_resources(DomDocument* document, Context* runtime_owner);

const char* dom_document_default_language(const DomDocument* document);
const char* dom_element_language(DomElement* element);
bool dom_document_set_content_language(DomDocument* document, const char* header);
bool dom_document_process_metadata_insertion(DomDocument* document, DomNode* subtree);
extern "C" void dom_css_document_language_changed(void* document);

// The parent iframe owns the embedded document's bounded upward edge. The
// document keeps a generation-checked reference rather than a raw node pointer.
bool dom_document_set_embedding(DomDocument* embedded, DomDocument* parent,
                                DomElement* iframe);
void dom_document_clear_embedding(DomDocument* embedded);
DomElement* dom_document_embedding_element(DomDocument* embedded);

class DisplayInner {
    static constexpr uint16_t list_item_bit = 1u << 15;
    static_assert(CSS_VALUE__LAST_ENTRY < list_item_bit, "display modes must leave room for list-item");
    uint16_t encoded_;

public:
    DisplayInner() = default;
    constexpr DisplayInner(CssEnum mode, bool list_item = false)
        : encoded_(static_cast<uint16_t>(mode) | (list_item ? list_item_bit : 0)) {}

    // layout checks read the mode; copies retain the list marker.
    constexpr CssEnum mode() const { return static_cast<CssEnum>(encoded_ & ~list_item_bit); }
    constexpr operator CssEnum() const { return mode(); }
    constexpr DisplayInner& operator=(CssEnum mode) {
        encoded_ = (encoded_ & list_item_bit) | static_cast<uint16_t>(mode);
        return *this;
    }
    constexpr bool list_item() const { return (encoded_ & list_item_bit) != 0; }
    constexpr void set_list_item(bool enabled) {
        encoded_ = (encoded_ & ~list_item_bit) | (enabled ? list_item_bit : 0);
    }
};
static_assert(sizeof(DisplayInner) == sizeof(CssEnum), "encoded display mode must fit in one CSS enum");

struct DisplayValue {
    CssEnum outer;
    DisplayInner inner;
};

typedef struct InlineProp InlineProp;
typedef struct FlexItemProp FlexFlowProp;
typedef struct GridItemProp GridItemProp;
typedef struct BoundaryProp BoundaryProp;
typedef struct BlockProp BlockProp;
struct BlockPropExt;
struct BlockContainmentProp;
typedef struct ScrollProp ScrollProp;
typedef struct PositionProp PositionProp;
typedef struct TransformProp TransformProp;
typedef struct FilterProp FilterProp;
typedef struct PseudoContentProp PseudoContentProp;
typedef struct EmbedStyle EmbedStyle;
typedef struct TableCellProp TableCellProp;
typedef struct TableProp TableProp;
typedef struct FormControlProp FormControlProp;
typedef struct ViewBlock ViewBlock;

extern const BlockProp BLOCK_PROP_DEFAULT;
extern const BoundaryProp BOUNDARY_PROP_DEFAULT;
extern const FontProp FONT_PROP_DEFAULT;
extern const InlineProp INLINE_PROP_DEFAULT;
extern const ScrollProp SCROLL_PROP_DEFAULT;
extern const PositionProp POSITION_PROP_DEFAULT;
extern const EmbedStyle EMBED_STYLE_DEFAULT;
extern const TransformProp TRANSFORM_PROP_DEFAULT;
extern const FilterProp FILTER_PROP_DEFAULT;
extern const MultiColumnProp MULTICOL_PROP_DEFAULT;
extern const FlexItemProp FLEX_ITEM_PROP_DEFAULT;
extern const GridItemProp GRID_ITEM_PROP_DEFAULT;
extern const TableProp TABLE_PROP_DEFAULT;
extern const TableCellProp TABLE_CELL_PROP_DEFAULT;
extern const FormControlProp FORM_CONTROL_PROP_DEFAULT;

// tier-1: doc-pool, survives relayout
typedef struct LayoutFragmentBox {
    float x, y, width, height;
    float flow_extent;  // consumed block flow can be smaller than visible monolithic overflow
    bool has_flow_extent;
    int fragment_index;
    int column_index;
    int row_index;
    lam::Own<struct LayoutFragmentBox> next;
} LayoutFragmentBox;

// Layout cache (from radiant/layout_cache.hpp)
namespace radiant { struct LayoutCache; }

// CSS Custom Property (CSS Variable) storage
// tier-1: doc-pool, survives relayout
struct CssCustomProp {
    lam::Up<const char> name;       // Variable name (e.g., "--primary-color")
    lam::Up<const CssValue> value;  // Variable value
    lam::Up<const char> value_text; // Raw value text for faithful CSSOM serialization
    size_t value_text_len;          // Length of value_text
    lam::Up<CssDeclaration> declaration; // cascade metadata and source-owner record
    const void* animation_owner; // effect identity; cancellation removes only its samples
    lam::Own<CssCustomProp> next;   // Linked list for simple storage
};

// Custom-property names retain their authored spelling, while var() parsing
// may omit the leading dashes from its lookup token.
bool css_custom_property_name_matches(const char* stored_name,
    const char* lookup_name, size_t stored_length = (size_t)-1,
    size_t lookup_length = (size_t)-1);
DomElement* dom_parent_element(DomElement* element);
DomNode* dom_source_inclusive_ancestor(DomNode* node);
DomNode* dom_source_parent(DomNode* node);
DomNode* dom_source_child(DomElement* element, bool first);
DomNode* dom_source_sibling(DomNode* node, bool forward);
DomNode* dom_source_tree_root(DomNode* node);
DomNode* dom_source_next_in_tree(DomNode* root, DomNode* node);
DomElement* dom_find_source_element_by_id(DomNode* root, const char* id);
DomElement* dom_details_first_summary(DomElement* details);
void dom_collect_text_content(DomNode* node, StrBuf* output);
const CssCustomProp* dom_element_lookup_own_custom_property_entry(DomElement* element,
    const char* name, size_t name_length = (size_t)-1, bool exclude_animations = false);
void dom_element_invalidate_custom_property_index(DomElement* element);
const CssValue* dom_element_lookup_own_custom_property(DomElement* element,
    const char* name, size_t name_length = (size_t)-1,
    StrView* token_text = nullptr);
const CssValue* dom_element_lookup_custom_property(DomElement* element,
    const char* name, DomElement** owner);

enum DomElementFlag : uint32_t {
    ELMT_FLAG_NEEDS_STYLE_RECOMPUTE = 1u << 0,
    ELMT_FLAG_STYLES_RESOLVED = 1u << 1,
    ELMT_FLAG_FLOAT_PRELAID = 1u << 2,
    ELMT_FLAG_HAS_CACHED_INTRINSIC_WIDTHS = 1u << 3,
    ELMT_FLAG_MEASURING_INTRINSIC_WIDTH = 1u << 4,
    ELMT_FLAG_HAS_PENDING_SCROLL_X = 1u << 5,
    ELMT_FLAG_HAS_PENDING_SCROLL_Y = 1u << 6,
    ELMT_FLAG_HTML_NAMESPACE = 1u << 7,
    ELMT_FLAG_LOCAL_NAME_IS_TAG = 1u << 8,
    ELMT_FLAG_LOCAL_NAME_IS_TAG_SUFFIX = 1u << 9,
    ELMT_FLAG_TAG_NAME_NONCANONICAL = 1u << 10,
    ELMT_FLAG_PARENT_ITEM_KIND_SHIFT = 11,
    ELMT_FLAG_PARENT_ITEM_KIND_MASK = 3u << ELMT_FLAG_PARENT_ITEM_KIND_SHIFT,
    ELMT_FLAG_ROLE_KIND_SHIFT = 13,
    ELMT_FLAG_ROLE_KIND_MASK = 7u << ELMT_FLAG_ROLE_KIND_SHIFT,
    ELMT_FLAG_SYNTHETIC = 1u << 16,
    // Canonical view props are immutable. Every writer must cross ensure_* so
    // the element can copy the value back into mutable prop-pool storage.
    ELMT_FLAG_INLINE_PROP_SHARED = 1u << 17,
    // Reserved for R6: shared specified styles are owned by a StyleEpoch pool.
    ELMT_FLAG_SPECIFIED_STYLE_SHARED = 1u << 18,
    // Option selectedness is live IDL state. Keep it on the native node so a
    // DOM reconciliation/layout pass can observe script writes without a JS
    // execution context or an expando-map lookup.
    ELMT_FLAG_OPTION_SELECTEDNESS_SET = 1u << 19,
    ELMT_FLAG_OPTION_SELECTEDNESS_VALUE = 1u << 20,
    // The spec's per-control "dirty" flags (dirty checkedness, dirty value,
    // option dirty selectedness, select dirtiness). They lived as JS expandos
    // on the wrapper, which made reading them a realm operation; they are
    // control state, so they live on the node like selectedness does.
    ELMT_FLAG_CHECKED_DIRTY  = 1u << 23,
    ELMT_FLAG_VALUE_DIRTY    = 1u << 24,
    ELMT_FLAG_OPTION_DIRTY   = 1u << 25,
    ELMT_FLAG_SELECT_DIRTY   = 1u << 26,
    // Generated pseudo boxes read their originating element's pseudo tree.
    // Borrowing is distinct from epoch sharing because it has no epoch refcount.
    ELMT_FLAG_SPECIFIED_STYLE_BORROWED = 1u << 21,
    // CSS Tables fixup boxes carry generated display roles rather than authored style.
    ELMT_FLAG_TABLE_FIXUP = 1u << 22,
    // HTML's "in table" form insertion creates a DOM node only for form-owner
    // resolution; it is not a rendered form box unless CSS authoring overrides it.
    ELMT_FLAG_PARSER_INSERTED_TABLE_FORM = 1u << 27,
    ELMT_FLAG_SCROLL_EVENT_PENDING = 1u << 28,
    ELMT_FLAG_USER_VALIDITY = 1u << 29,
    // A pass-local element in layout scratch (an anonymous flex text item): its
    // props live in that scratch too, so it never reaches the view-tree pool.
    ELMT_FLAG_SCRATCH_LIVED = 1u << 30,
    // An intrinsic measurement walked this element and re-resolved its style
    // against the measured width; the next committed pass resolves it again.
    ELMT_FLAG_MEASURE_RESTYLED = 1u << 31,
};

static_assert((ELMT_FLAG_INLINE_PROP_SHARED & ((1u << 17) - 1u)) == 0,
              "view-prop ownership flags overlap established element state");
static_assert((ELMT_FLAG_SPECIFIED_STYLE_SHARED & ELMT_FLAG_INLINE_PROP_SHARED) == 0,
              "style and view-prop ownership flags must remain independent");
static_assert((ELMT_FLAG_SPECIFIED_STYLE_BORROWED & ELMT_FLAG_SPECIFIED_STYLE_SHARED) == 0,
              "borrowed and epoch-shared styles must remain independent");

enum FragmentUnionKind : uint8_t {
    FRAGMENT_UNION_INLINE = 0,
    FRAGMENT_UNION_ANCESTOR,
    FRAGMENT_UNION_COLLAPSED_LINE,
    FRAGMENT_UNION_SPLIT_INLINE,
    FRAGMENT_UNION_INLINE_CB,
    FRAGMENT_UNION_LINE_BOX,
    FRAGMENT_UNION_COUNT,
};

enum PseudoStyleKind : uint8_t {
    PSEUDO_STYLE_BEFORE = 0,
    PSEUDO_STYLE_AFTER,
    PSEUDO_STYLE_FIRST_LETTER,
    PSEUDO_STYLE_MARKER,
    PSEUDO_STYLE_PLACEHOLDER,
    PSEUDO_STYLE_BACKDROP,
    // keep existing slot indices stable; first-line was added after the
    // originally supported pseudo-style slots.
    PSEUDO_STYLE_FIRST_LINE,
    PSEUDO_STYLE_FILE_SELECTOR_BUTTON,
    // CSS GCPM 3 §2.3: generated by the page flow for float: footnote elements
    PSEUDO_STYLE_FOOTNOTE_CALL,
    PSEUDO_STYLE_FOOTNOTE_MARKER,
    PSEUDO_STYLE_COUNT,
};

// tier-1: doc-pool, survives relayout
struct FragmentUnion {
    float min_x, max_x, min_y, max_y;
    int first_source_line, last_source_line; // one-based line IDs; zero means no recorded source fragment
};

// Dynamic DOM nodes have no Mark-backed Element.  Their attributes therefore
// live in document-pool storage while retaining the same public DOM API as
// parsed nodes.
struct DomSyntheticAttribute {
    lam::Own<const char> name;   // document-pool copies
    lam::Own<const char> value;
};

// Highlight declarations borrow stylesheet values while preserving the
// element-specific cascade inputs without allocating a full style tree.
struct CssSelectionCascadeValue {
    lam::Up<CssDeclaration> source;
    CssSpecificity specificity;
    CssOrigin origin;
    uint32_t scope_proximity;
};

struct CssSelectionStyle {
    CssSelectionCascadeValue color;
    CssSelectionCascadeValue background_color;
};

struct DomNamespacedAttribute {
    lam::Up<const char> namespace_uri;   // physical source Input name pool
    lam::Own<const char> local_name;
    lam::Own<const char> qualified_name;
    lam::Own<const char> value; // null preserves a native Mark null attribute (S2.2.3)
    bool active;
    lam::Own<DomNamespacedAttribute> next;
};

static inline bool dom_attribute_is_internal(const char* name) {
    return element_attribute_is_internal(name);
}

// borrowed attribute fields; iteration preserves expanded identity and native null values.
struct DomAttributeView {
    const char* namespace_uri;
    const char* qualified_name;
    const char* local_name;
    const char* value;
};
struct DomAttributeIterator {
    DomElement* element;
    const char** names;
    int count;
    int index;
    DomNamespacedAttribute* next;
    bool recorded;
};
DomAttributeIterator dom_element_attribute_iterator(DomElement* element);
bool dom_element_next_attribute(DomAttributeIterator* iterator, DomAttributeView* attribute);
bool dom_element_set_namespace_identity(DomElement* element,
    const char* namespace_uri, const char* local_name);

DomNamespacedAttribute* dom_element_namespaced_attributes(DomElement* element);
bool dom_element_record_namespaced_attribute(DomElement* element,
    const char* namespace_uri, const char* qualified_name, const char* value);
void dom_element_remove_namespaced_attribute(DomElement* element,
    const char* namespace_uri, const char* local_name);
const char* dom_element_get_namespaced_attribute(DomElement* element,
    const char* namespace_uri, const char* local_name);
const char* dom_element_attribute_value_ns(DomElement* element,
    const char* namespace_uri, const char* local_name);
const char* dom_element_find_qualified_attribute(DomElement* element,
    const char* namespace_uri, const char* local_name);
bool dom_element_set_attribute_ns(DomElement* element,
    const char* namespace_uri, const char* qualified_name, const char* value, bool replace_name = false);
bool dom_element_remove_attribute_ns(DomElement* element,
    const char* namespace_uri, const char* local_name);

DomAttr* dom_attribute_node_create(DomDocument* document, const char* namespace_uri,
    const char* qualified_name, const char* value);
DomAttr* dom_element_attribute_node(DomElement* element, const char* name);
DomAttr* dom_element_attribute_node_ns(DomElement* element,
    const char* namespace_uri, const char* local_name);
bool dom_attribute_node_set_value(DomAttr* attribute, const char* value);
void dom_attribute_node_attach(DomElement* element, DomAttr* attribute);
void dom_attribute_node_detach(DomAttr* attribute);
void dom_attribute_node_release_retired_storage(DomAttr* attribute);

// Property slots on the element or its extensions. View teardown checks the
// view-owned slots; the DOM retires document-owned groups (D5.5.1).
#define DOM_ELEMENT_VIEW_SLOTS(X) \
    X(font) X(in_line) X(bound) X(blk) X(scroller) X(embed) X(position) \
    X(transform) X(pseudo) X(layout_cache) X(fi) X(gi) X(tb) X(td) X(form) \
    X(multicol) X(vpath) X(filter) X(backdrop_filter) X(marker) X(layout_fragments) X(popover) X(shadow)
enum class DomViewSlot {
#define DOM_VIEW_SLOT_ENUM(name) name,
    DOM_ELEMENT_VIEW_SLOTS(DOM_VIEW_SLOT_ENUM)
#undef DOM_VIEW_SLOT_ENUM
    count
};

// tier-1: authored pseudo styles survive the view that borrows them.
struct PseudoStyle {
    lam::Own<StyleTree> styles[PSEUDO_STYLE_COUNT];
};

// the node's storage pool owns this directory, which borrows the namespace URI.
struct DomStyleExt {
    lam::ViewProp<TableProp, DomViewSlot::tb> tb;
    lam::ViewProp<FormControlProp, DomViewSlot::form> form;
    lam::ViewProp<TransformProp, DomViewSlot::transform> transform;
    lam::ViewProp<PseudoContentProp, DomViewSlot::pseudo> pseudo;
    lam::Up<const char> namespace_uri;
    lam::Own<PseudoStyle> pseudo_styles;

    bool empty() const { return !tb && !form && !transform && !pseudo && !namespace_uri && !pseudo_styles; }
};

// tier-1: attribute storage follows the element through adoption.
struct DomAttributeCache {
    lam::OwnArr<const char*> attribute_names_cache;
    int attribute_names_capacity;
    lam::OwnArr<DomSyntheticAttribute> synthetic_attributes;
    int synthetic_attribute_count;
    int synthetic_attribute_capacity;
    lam::Own<DomNamespacedAttribute> namespaced_attributes;
    bool attributes_are_recorded; // ordered expanded names become authoritative after namespace mutation
};

// tier-1 storage for view geometry; reset when view geometry is retired.
struct DomFragmentGeometry {
    FragmentUnion frags[FRAGMENT_UNION_COUNT];
    uint8_t fragment_presence_mask;
    bool has_inline_cb_edge_snapshot;
    float ruby_column_start_offset;
    float ruby_column_end_offset;
    float ruby_empty_base_block_size;
    bool has_ruby_column_geometry;
    bool has_empty_ruby_base;
    bool has_ruby_parallel_fragments; // selected text rectangles own each line's allocation
};

// task generations suppress stale toggle events after state changes.
struct DomToggleTaskTracker {
    uint32_t generation;
    bool pending;
    bool old_open;
};

// Document-owned state for popovers and their shared top-layer placement.
struct PopoverStyle {
    DomNodeRef implicit_anchor;
    uint64_t order; // document-wide opening sequence, with zero reserved
    DomToggleTaskTracker toggle;
    bool in_top_layer : 1;
    bool open : 1;
    bool hiding : 1;
    DomPopoverMode attribute_mode : 2;
    DomPopoverMode opened_mode : 2;
};

// Document-owned links and metadata for hosts, roots, and scoped stylesheets.
struct ShadowStyle {
    lam::Up<DomElement> host;
    lam::Own<DomElement> root;
    lam::Up<CssStylesheet> inline_stylesheet;
    int64_t source_position; // one plus the declaration's parsed child index
    bool closed : 1;
    bool delegates_focus : 1;
    bool manual_slot_assignment : 1;
    bool clonable : 1;
    bool serializable : 1;
    bool declarative : 1;
};

// tier-1: doc-pool, survives relayout
struct DomStyleRareExt {
    // adoption changes ownerDocument, not the pool owning reclaimable metadata (D5.5.1).
    lam::Up<DomDocument> storage_document;
    NameId name_id;  // caches the catalog ID of a namespaced tag’s local name
    // pack node state in the alignment gap before the pointer slots.
    bool has_last_remembered_width : 1;
    bool has_last_remembered_height : 1;
    bool has_animated_display : 1;
    bool dialog_modal : 1;
    bool dialog_cancel_dispatching : 1;
    bool form_constructing_entries : 1;
    bool form_firing_submission_events : 1;
    bool selected_content_pending : 1;
    bool selected_content_reset_pending : 1;
    bool script_already_started : 1;
    bool script_parser_pending : 1;
    bool selectionchange_event_pending : 1;
    lam::Own<DomFragmentGeometry> fragment_geometry;
    lam::Own<DomAttributeCache> attributes;
    lam::ViewProp<MultiColumnProp, DomViewSlot::multicol> multicol;
    lam::ViewProp<VectorPathProp, DomViewSlot::vpath> vpath;
    lam::ViewProp<FilterProp, DomViewSlot::filter> filter;
    lam::ViewProp<FilterProp, DomViewSlot::backdrop_filter> backdrop_filter;
    lam::Own<CssTransitionElemState> transition_state;
    lam::Up<CustomLayoutPaintState> custom_layout_paint;  // a document resource's paint state
    lam::ViewProp<LayoutFragmentBox, DomViewSlot::layout_fragments> layout_fragments;  // fragment list
    int layout_fragment_count;
    float last_remembered_width;
    float last_remembered_height;
    DisplayValue animated_display; // fills the gap before the animation pointer
    lam::Own<CssWebAnimationState> web_animation_state;  // head of the element's effect list
    lam::ViewProp<ShadowStyle, DomViewSlot::shadow> shadow;
    lam::Own<DomManualSlotLink> manual_slot_first;
    lam::Up<DomManualSlotLink> manual_slot_last;
    lam::OwnArr<DomNodeRef> signaled_slot_assignment;
    size_t signaled_slot_assignment_count;
    float pending_element_scroll_x;
    float pending_element_scroll_y;
    lam::Up<DomAttr> attribute_nodes;
    lam::Up<struct DomInlineDeclarations> inline_declarations; // document resource; authored CSSOM block
    lam::Own<const char> dialog_return_value; // document-pooled WebIDL state, independent of JS expandos
    float form_image_selected_x; // image-relative CSS pixels, rounded at submission
    float form_image_selected_y;
    lam::ViewProp<PopoverStyle, DomViewSlot::popover> popover;
    DomNodeRef dialog_previous_focus;
    DomToggleTaskTracker dialog_toggle;
    // deferred text-control events keep their queue links across layout teardown.
    lam::Up<DomElement> selectionchange_event_next;
    // ::marker layout state; markers have no BlockProp
    lam::ViewProp<MarkerProp, DomViewSlot::marker> marker;
    lam::Own<CssCustomProp> css_variables;
    lam::Own<struct CssCustomPropertyIndex> custom_property_index;
};

struct DomClassList {
    int class_count;
    const char* class_names[];

    static DomClassList* allocate(Pool* pool, int capacity);
};

/**
 * DomElement - DOM element with integrated CSS styling
 *
 * This structure extends the basic DOM element concept with:
 * - Specified style tree (AVL tree of CSS declarations from rules)
 * - Computed style tree (AVL tree of resolved CSS values)
 * - Version tracking for cache invalidation
 * - Parent/child relationships for inheritance
 */
// tier-1: doc-pool, survives relayout
struct DomElement : DomNode {
    enum ParentItemKind : uint8_t {
        PARENT_ITEM_NONE = 0,
        PARENT_ITEM_FLEX = 1,
        PARENT_ITEM_GRID = 2,
    };

    enum RoleKind : uint8_t {
        ROLE_NONE = 0,
        ROLE_TABLE = 1,
        ROLE_CELL = 2,
        ROLE_FORM = 3,
    };

    // Resolved props point into the shorter-lived view pool by unified-tree design;
    // every relayout resets and rebuilds them before consumers may read the tree.
    // The element owns each prop it points to (Own<T>). in_line and
    // specified_style are Shared<T>: a canonical value shared with other
    // elements, or the element's private copy after copy-on-write (recorded by
    // the inline_prop_shared flag and the style epoch). A scratch-lived
    // anonymous flex item borrows another element's FontProp in font; it never
    // reaches view-tree teardown.

    // === Embedded Lambda Element (at known offset from DomNode base) ===
    // In UI mode, this IS the Lambda Element. Otherwise, data is copied from the
    // original Element during create(). MarkEditor operates on
    // this embedded value, so mutations happen in-place.
    Element elmt;

    // Tree structure (only elements can have children)
    lam::Own<DomNode> first_child;  // First child node (Element, Text, or Comment); owns the sibling chain
    lam::Up<DomNode> last_child;    // Last child node
    // document reference (provides Arena and Input*)
    lam::Up<DomDocument> doc;    // Parent document (provides arena and input)

    // HTML/CSS style related
    lam::Up<const char> id;      // Element ID attribute (cached)
    lam::Own<DomClassList> classes; // cached class count and name pointers share one allocation
    NameId tag_id;               // Generated markup identity; custom tags use NAME_ID_NONE.
    uint32_t elmt_flags;         // compact element state; use the accessors below

    // Bumped by dom_mutation_notify on every element at or above a mutation, so the painter
    // can tell an unchanged inline SVG or paint layer (a reusable raster) from a changed one.
    // A non-inherited presentation write skips its own target: it moves or fades that
    // element's raster without changing it (Radiant_Design_Animation_Repaint §4 B2).
    uint32_t layer_generation;
    uint32_t intrinsic_input_revision; // subtree dirty notifications; fills existing pointer padding

    // Version tracking for cache invalidation
    uint32_t style_version;      // Incremented when specified styles change
    DisplayValue display;

    lam::Shared<StyleTree> specified_style;  // Specified values from CSS rules (AVL tree)
    // we do not store computed_style;

    // CSS Text soft hyphen fragments can contribute to an inline element's
    // border-box union without producing an additional DOM text rect.

    // collapsed text can create an anonymous line fragment that affects ancestor
    // inline decorations without contributing to this element's own DOMRect.

    // line-edge collapsible whitespace may leave a zero-width inline fragment
    // on a real line even though its text node has no visible rect.

    // block-in-inline splitting creates anonymous line fragments for every inline
    // ancestor in the split chain. These store the content-area union; each span's
    // own border/padding is applied when its DOMRect is computed.

    // view related fields

    // span properties
    lam::ViewProp<FontProp, DomViewSlot::font> font;  // font style
    lam::ViewProp<BoundaryProp, DomViewSlot::bound> bound;  // block boundary properties
    lam::ViewRef<InlineProp, DomViewSlot::in_line> in_line;  // private copy or canonical entry (inline_prop_shared)  // inline specific style properties

    // block properties
    float content_width, content_height;  // width and height of the child content including padding
    lam::ViewProp<BlockProp, DomViewSlot::blk> blk;  // block specific style properties
    lam::ViewProp<ScrollProp, DomViewSlot::scroller> scroller;  // handles overflow
    // document-pool storage for flexbox, grid, images, and embedded documents
    lam::Own<EmbedStyle> embed;
    // positioning properties for CSS positioning
    lam::ViewProp<PositionProp, DomViewSlot::position> position;
    // Parent-item and own-role storage are independent because CSS permits a
    // table or form control to participate in a flex/grid parent. Table
    // structure and form-control state are also independent: a replaced form
    // control can occupy a table-internal grid slot without losing its form
    // behavior.
    // Reads must enter through the tagged accessors below; direct members are
    // reserved for mutation after the corresponding tag has been established.
    union {
        lam::ViewProp<FlexItemProp, DomViewSlot::fi> fi;
        lam::ViewProp<GridItemProp, DomViewSlot::gi> gi;
    };
    lam::ViewProp<TableCellProp, DomViewSlot::td> td;  // table cell specific properties
    lam::Own<DomStyleExt> style_ext;
    lam::Own<DomStyleRareExt> ext; // rare DOM/view state, allocated lazily from doc pool

    // vector path for PDF/SVG curve rendering
    // Layout cache for avoiding redundant layout computations (Taffy-inspired)
    // Stores up to 9 measurement results + 1 final layout result
    lam::ViewProp<radiant::LayoutCache, DomViewSlot::layout_cache> layout_cache;

    // Factories rely on zeroed arena/pool storage and write only semantic non-zero fields.
    static DomElement* create(DomDocument* doc, const char* tag_name, Element* backing);
    static DomElement* create_in(Arena* arena);
    static DomElement* create_in(Pool* pool);
    static DomElement* create_in(DomElement* storage, DomDocument* doc,
                                 const char* tag_name, Element* backing);

    int class_count() const { return classes ? classes->class_count : 0; }
    const char* const* class_names() const { return classes ? classes->class_names : nullptr; }

    void advance_style_version() {
        style_version++;
        if (doc) doc->style_query_epoch++;
    }
    DomDocument* storage_owner() const {
        return ext && ext->storage_document ? ext->storage_document.get() : doc.get();
    }
    Pool* storage_pool() const {
        DomDocument* owner = storage_owner();
        return owner ? owner->document_pool.get() : nullptr;
    }
    bool preserve_storage_owner() {
        DomStyleRareExt* data = ensure_ext();
        if (!data) return false;
        if (!data->storage_document) data->storage_document = doc;
        return true;
    }

    bool flag(DomElementFlag value) const { return (elmt_flags & value) != 0; }
    void set_flag(DomElementFlag value, bool enabled) {
        if (enabled) elmt_flags |= value;
        else elmt_flags &= ~value;
    }

    bool needs_style_recompute() const { return flag(ELMT_FLAG_NEEDS_STYLE_RECOMPUTE); }
    void invalidate_intrinsic_inputs() {
        // text and style edits both invalidate every containing size/flow query.
        for (DomNode* node = this; node; node = node->parent) {
            if (node->is_element()) {
                DomElement* element = node->as_element();
                element->intrinsic_input_revision++;
                element->set_has_cached_intrinsic_widths(false);
            }
        }
    }
    void set_needs_style_recompute(bool value) {
        set_flag(ELMT_FLAG_NEEDS_STYLE_RECOMPUTE, value);
        // A style mutation may change intrinsic contributions before the next
        // retained layout generation; do not reuse its provisional measurement.
        if (value) {
            // explicit invalidation also expires computed CSS before layout commits.
            style_version++;
            // anonymous-node initialization must not expire unrelated subtree queries.
            invalidate_intrinsic_inputs();
        }
    }
    bool styles_resolved() const { return flag(ELMT_FLAG_STYLES_RESOLVED); }
    bool display_resolved() const {
        return display.outer != CSS_VALUE__UNDEF && !needs_style_recompute();
    }
    void set_styles_resolved(bool value) {
        set_flag(ELMT_FLAG_STYLES_RESOLVED, value);
        // A reset discards the cascade that produced any cached contribution.
        if (!value) set_has_cached_intrinsic_widths(false);
    }
    bool measure_restyled() const { return flag(ELMT_FLAG_MEASURE_RESTYLED); }
    void set_measure_restyled(bool value) { set_flag(ELMT_FLAG_MEASURE_RESTYLED, value); }
    bool float_prelaid() const { return flag(ELMT_FLAG_FLOAT_PRELAID); }
    void set_float_prelaid(bool value) { set_flag(ELMT_FLAG_FLOAT_PRELAID, value); }
    bool has_cached_intrinsic_widths() const { return flag(ELMT_FLAG_HAS_CACHED_INTRINSIC_WIDTHS); }
    void set_has_cached_intrinsic_widths(bool value) { set_flag(ELMT_FLAG_HAS_CACHED_INTRINSIC_WIDTHS, value); }
    bool measuring_intrinsic_width() const { return flag(ELMT_FLAG_MEASURING_INTRINSIC_WIDTH); }
    void set_measuring_intrinsic_width(bool value) { set_flag(ELMT_FLAG_MEASURING_INTRINSIC_WIDTH, value); }
    bool is_synthetic() const { return flag(ELMT_FLAG_SYNTHETIC); }
    void set_synthetic(bool value) { set_flag(ELMT_FLAG_SYNTHETIC, value); }
    bool is_table_fixup() const { return flag(ELMT_FLAG_TABLE_FIXUP); }
    void set_table_fixup(bool value) { set_flag(ELMT_FLAG_TABLE_FIXUP, value); }
    bool is_parser_inserted_table_form() const {
        return flag(ELMT_FLAG_PARSER_INSERTED_TABLE_FORM);
    }
    void set_parser_inserted_table_form(bool value) {
        set_flag(ELMT_FLAG_PARSER_INSERTED_TABLE_FORM, value);
    }
    bool scratch_lived() const { return flag(ELMT_FLAG_SCRATCH_LIVED); }
    void mark_scratch_lived() { set_flag(ELMT_FLAG_SCRATCH_LIVED, true); }
    bool inline_prop_shared() const { return flag(ELMT_FLAG_INLINE_PROP_SHARED); }
    void mark_inline_prop_owned() { set_flag(ELMT_FLAG_INLINE_PROP_SHARED, false); }
    void mark_inline_prop_shared() { set_flag(ELMT_FLAG_INLINE_PROP_SHARED, true); }
    void clear_inline_prop_binding() {
        in_line = nullptr;
        set_flag(ELMT_FLAG_INLINE_PROP_SHARED, false);
    }
    bool specified_style_shared() const { return flag(ELMT_FLAG_SPECIFIED_STYLE_SHARED); }
    bool specified_style_borrowed() const { return flag(ELMT_FLAG_SPECIFIED_STYLE_BORROWED); }
    void mark_specified_style_owned() {
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_SHARED, false);
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_BORROWED, false);
    }
    void mark_specified_style_shared() {
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_BORROWED, false);
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_SHARED, true);
    }
    void mark_specified_style_borrowed() {
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_SHARED, false);
        set_flag(ELMT_FLAG_SPECIFIED_STYLE_BORROWED, true);
    }
    bool has_pending_element_scroll_x() const { return flag(ELMT_FLAG_HAS_PENDING_SCROLL_X); }
    void set_has_pending_element_scroll_x(bool value) { set_flag(ELMT_FLAG_HAS_PENDING_SCROLL_X, value); }
    bool has_pending_element_scroll_y() const { return flag(ELMT_FLAG_HAS_PENDING_SCROLL_Y); }
    void set_has_pending_element_scroll_y(bool value) { set_flag(ELMT_FLAG_HAS_PENDING_SCROLL_Y, value); }
    bool scroll_event_pending() const { return flag(ELMT_FLAG_SCROLL_EVENT_PENDING); }
    void set_scroll_event_pending(bool value) { set_flag(ELMT_FLAG_SCROLL_EVENT_PENDING, value); }
    bool checked_dirty() const { return flag(ELMT_FLAG_CHECKED_DIRTY); }
    void set_checked_dirty(bool v) { set_flag(ELMT_FLAG_CHECKED_DIRTY, v); }
    bool value_dirty() const { return flag(ELMT_FLAG_VALUE_DIRTY); }
    void set_value_dirty(bool v) { set_flag(ELMT_FLAG_VALUE_DIRTY, v); }
    bool option_dirty() const { return flag(ELMT_FLAG_OPTION_DIRTY); }
    void set_option_dirty(bool v) { set_flag(ELMT_FLAG_OPTION_DIRTY, v); }
    bool select_dirty() const { return flag(ELMT_FLAG_SELECT_DIRTY); }
    void set_select_dirty(bool v) { set_flag(ELMT_FLAG_SELECT_DIRTY, v); }
    bool user_validity() const { return flag(ELMT_FLAG_USER_VALIDITY); }
    void set_user_validity(bool v) { set_flag(ELMT_FLAG_USER_VALIDITY, v); }
    bool has_option_selectedness() const { return flag(ELMT_FLAG_OPTION_SELECTEDNESS_SET); }
    bool option_selectedness() const { return flag(ELMT_FLAG_OPTION_SELECTEDNESS_VALUE); }
    void set_option_selectedness(bool value) {
        set_flag(ELMT_FLAG_OPTION_SELECTEDNESS_SET, true);
        set_flag(ELMT_FLAG_OPTION_SELECTEDNESS_VALUE, value);
    }
    bool has_inline_fragment_union() const { return has_fragment_union(FRAGMENT_UNION_INLINE); }
    void set_has_inline_fragment_union(bool value) { set_has_fragment_union(FRAGMENT_UNION_INLINE, value); }
    bool has_ancestor_fragment_union() const { return has_fragment_union(FRAGMENT_UNION_ANCESTOR); }
    void set_has_ancestor_fragment_union(bool value) { set_has_fragment_union(FRAGMENT_UNION_ANCESTOR, value); }
    bool has_collapsed_line_fragment_union() const { return has_fragment_union(FRAGMENT_UNION_COLLAPSED_LINE); }
    void set_has_collapsed_line_fragment_union(bool value) { set_has_fragment_union(FRAGMENT_UNION_COLLAPSED_LINE, value); }
    bool has_split_inline_fragment_union() const { return has_fragment_union(FRAGMENT_UNION_SPLIT_INLINE); }
    void set_has_split_inline_fragment_union(bool value) { set_has_fragment_union(FRAGMENT_UNION_SPLIT_INLINE, value); }
    bool has_inline_cb_edge_snapshot() const { return fragment_geometry() && fragment_geometry()->has_inline_cb_edge_snapshot; }
    void set_has_inline_cb_edge_snapshot(bool value) {
        DomFragmentGeometry* data = value ? ensure_fragment_geometry() : fragment_geometry();
        if (data) data->has_inline_cb_edge_snapshot = value;
    }

    ParentItemKind parent_item_kind() const {
        return (ParentItemKind)((elmt_flags & ELMT_FLAG_PARENT_ITEM_KIND_MASK) >>
                                ELMT_FLAG_PARENT_ITEM_KIND_SHIFT);
    }
    void set_parent_item_kind(ParentItemKind kind) {
        elmt_flags = (elmt_flags & ~ELMT_FLAG_PARENT_ITEM_KIND_MASK) |
                     ((uint32_t)kind << ELMT_FLAG_PARENT_ITEM_KIND_SHIFT);
    }
    RoleKind role_kind() const {
        return (RoleKind)((elmt_flags & ELMT_FLAG_ROLE_KIND_MASK) >> ELMT_FLAG_ROLE_KIND_SHIFT);
    }
    void set_role_kind(RoleKind kind) {
        elmt_flags = (elmt_flags & ~ELMT_FLAG_ROLE_KIND_MASK) |
                     ((uint32_t)kind << ELMT_FLAG_ROLE_KIND_SHIFT);
    }
    int item_prop_debug_kind() const {
        return role_kind() != ROLE_NONE ? 2 + (int)role_kind() : (int)parent_item_kind();
    }
    FlexItemProp* flex_item() const { return parent_item_kind() == PARENT_ITEM_FLEX ? fi : nullptr; }
    GridItemProp* grid_item() const { return parent_item_kind() == PARENT_ITEM_GRID ? gi : nullptr; }
    TableProp* table_storage() const { return style_ext ? style_ext->tb.get() : nullptr; }
    TableProp* table_prop() const { return role_kind() == ROLE_TABLE ? table_storage() : nullptr; }
    TableCellProp* cell_prop() const { return role_kind() == ROLE_CELL ? td : nullptr; }
    FormControlProp* form_storage() const { return style_ext ? style_ext->form.get() : nullptr; }
    FormControlProp* form_control() const {
        RoleKind role = role_kind();
        return (role == ROLE_FORM || role == ROLE_TABLE || role == ROLE_CELL) ? form_storage() : nullptr;
    }
    TransformProp* transform_prop() const { return style_ext ? style_ext->transform.get() : nullptr; }
    PseudoContentProp* pseudo_prop() const { return style_ext ? style_ext->pseudo.get() : nullptr; }

    DomStyleExt* ensure_style_ext();
    void release_style_ext();
    bool set_form_storage(FormControlProp* value) {
        DomStyleExt* slots = value ? ensure_style_ext() : style_ext.get();
        if (slots) slots->form = lam::view_prop(value);
        return slots || !value;
    }

    const BlockProp* block() const;
    const BoundaryProp* boundary() const;
    const FontProp* fontp() const;
    const InlineProp* inl() const;
    const ScrollProp* scroll() const;
    const PositionProp* positionp() const;
    const EmbedStyle* embedp() const;
    const TransformProp* transformp() const;
    const FilterProp* filterp() const;
    BlockProp* block_mut();
    BoundaryProp* boundary_mut();
    FontProp* font_mut();
    InlineProp* inline_mut();
    ScrollProp* scroll_mut();
    PositionProp* position_mut();
    EmbedStyle* embed_mut();
    TransformProp* transform_mut();
    FilterProp* filter_mut();

    BlockProp* ensure_block(ViewTree* tree);
    BoundaryProp* ensure_boundary(ViewTree* tree);
    FontProp* ensure_font(ViewTree* tree);
    InlineProp* ensure_inline(ViewTree* tree);
    ScrollProp* ensure_scroll(ViewTree* tree);
    PositionProp* ensure_position(ViewTree* tree);
    EmbedStyle* ensure_embed();
    TransformProp* ensure_transform(ViewTree* tree);
    FilterProp* ensure_filter(ViewTree* tree);
    MultiColumnProp* ensure_multicol(ViewTree* tree);
    FlexItemProp* ensure_flex_item(ViewTree* tree);
    GridItemProp* ensure_grid_item(ViewTree* tree);
    TableProp* ensure_table(ViewTree* tree);
    TableCellProp* ensure_cell(ViewTree* tree);
    FormControlProp* ensure_form(ViewTree* tree);
    PseudoContentProp* ensure_pseudo(LayoutContext* lycon);

    BoundaryProp* ensure_boundary(LayoutContext* lycon);
    BlockProp* ensure_block(LayoutContext* lycon);
    BlockPropExt* ensure_block_extras(LayoutContext* lycon);
    BlockContainmentProp* ensure_block_containment(LayoutContext* lycon);
    FontProp* ensure_font(LayoutContext* lycon);
    InlineProp* ensure_inline(LayoutContext* lycon);
    ScrollProp* ensure_scroll(LayoutContext* lycon);
    PositionProp* ensure_position(LayoutContext* lycon);
    TransformProp* ensure_transform(LayoutContext* lycon);
    FilterProp* ensure_filter(LayoutContext* lycon);
    MultiColumnProp* ensure_multicol(LayoutContext* lycon);

    bool set_attribute(const char* name, const char* value, bool preserve_case = false);
    bool set_attribute(NameId name_id, const char* value);
    const char* get_attribute(const char* name);
    const char* get_attribute(NameId name_id);
    const char* local_name() const;
    bool remove_attribute(const char* name);
    bool remove_attribute(NameId name_id);
    bool has_attribute(const char* name);
    bool has_attribute(NameId name_id);
    const char** attribute_names(int* count);
    bool add_class(const char* class_name);
    bool remove_class(const char* class_name);
    bool has_class(const char* class_name) const;
    bool toggle_class(const char* class_name);
    DomElement* parent_element() const;
    DomElement* first_child_element() const;
    DomElement* last_child_element() const;
    DomElement* next_sibling_element() const;
    DomElement* prev_sibling_element() const;
    using DomNode::append_child;
    using DomNode::insert_before;
    using DomNode::remove_child;
    bool link_child(DomElement* child);
    bool append_child(DomElement* child);
    bool remove_child(DomElement* child);
    bool insert_before(DomElement* new_child, DomElement* reference_child);
    bool is_first_child();
    bool is_last_child();
    bool is_only_child();
    int child_index();
    int child_count();
    int count_child_elements();
    bool matches_nth_child(int a, int b);
    DomText* append_text(const char* text_content);
    DomComment* append_comment(const char* comment_content);

    DomStyleRareExt* ensure_ext() {
        if (!ext && doc && storage_pool()) {
            ext = lam::own((DomStyleRareExt*)pool_calloc(storage_pool(), sizeof(DomStyleRareExt)));
            if (ext) doc->services.ext_allocations++;
        }
        return ext;
    }
    template<class T, class Slot>
    T* ensure_ext_group(Slot DomStyleRareExt::*slot) {
        DomStyleRareExt* data = ensure_ext();
        if (!data) return nullptr;
        Slot& group = data->*slot;
        if (!group) group = Slot((T*)pool_calloc(storage_pool(), sizeof(T)));
        return group;
    }
    DomAttributeCache* attribute_cache() const { return ext ? ext->attributes.get() : nullptr; }
    DomAttributeCache* ensure_attribute_cache() { return ensure_ext_group<DomAttributeCache>(&DomStyleRareExt::attributes); }
    bool has_recorded_attributes() const {
        const DomAttributeCache* cache = attribute_cache();
        return cache && cache->attributes_are_recorded;
    }
    DomFragmentGeometry* fragment_geometry() const { return ext ? ext->fragment_geometry.get() : nullptr; }
    DomFragmentGeometry* ensure_fragment_geometry() { return ensure_ext_group<DomFragmentGeometry>(&DomStyleRareExt::fragment_geometry); }
    PseudoStyle* pseudo_style_group() const { return style_ext ? style_ext->pseudo_styles.get() : nullptr; }
    StyleTree* pseudo_style(PseudoStyleKind kind) const {
        if (node_flags & DOM_NODE_FLAG_PRIVATE_PSEUDO_STYLES) {
            if (StyleTree* style = style_ext->pseudo_styles->styles[kind]) return style;
        }
        return shared_pseudo_reset_slots && kind <= PSEUDO_STYLE_AFTER
            ? shared_pseudo_style(kind) : nullptr;
    }
    unsigned shared_pseudo_reset_slot(PseudoStyleKind kind) const;
    void set_shared_pseudo_reset_slot(PseudoStyleKind kind, unsigned slot);
    StyleTree* shared_pseudo_style(PseudoStyleKind kind) const;
    PseudoStyle* ensure_pseudo_style_group();
    void release_pseudo_style_group();
    lam::Own<StyleTree>* pseudo_style_slot(PseudoStyleKind kind);
    void set_pseudo_style(PseudoStyleKind kind, StyleTree* style);
    bool has_fragment_union(FragmentUnionKind kind) const {
        const DomFragmentGeometry* geometry = fragment_geometry();
        return geometry && (geometry->fragment_presence_mask & (1u << kind));
    }
    void set_has_fragment_union(FragmentUnionKind kind, bool value) {
        DomFragmentGeometry* data = value ? ensure_fragment_geometry() : fragment_geometry();
        if (!data) return;
        if (value) data->fragment_presence_mask |= (1u << kind);
        else data->fragment_presence_mask &= ~(1u << kind);
    }
    const FragmentUnion* fragment_union(FragmentUnionKind kind) const {
        const DomFragmentGeometry* geometry = fragment_geometry();
        return geometry ? &geometry->frags[kind] : nullptr;
    }
    FragmentUnion* ensure_fragment_union(FragmentUnionKind kind) {
        DomFragmentGeometry* data = ensure_fragment_geometry();
        return data ? &data->frags[kind] : nullptr;
    }
    MultiColumnProp* multicol_prop() const { return ext ? ext->multicol : nullptr; }
    MarkerProp* marker_prop() const { return ext ? ext->marker : nullptr; }
    void set_marker_prop(MarkerProp* value) { if (value || ext) ensure_ext()->marker = lam::view_prop(value); }
    void set_multicol_prop(MultiColumnProp* value) { if (value || ext) ensure_ext()->multicol = lam::view_prop(value); }
    VectorPathProp* vector_path() const { return ext ? ext->vpath : nullptr; }
    void set_vector_path(VectorPathProp* value) { if (value || ext) ensure_ext()->vpath = lam::view_prop(value); }
    FilterProp* filter_prop() const { return ext ? ext->filter : nullptr; }
    void set_filter_prop(FilterProp* value) { if (value || ext) ensure_ext()->filter = lam::view_prop(value); }
    FilterProp* backdrop_filter_prop() const { return ext ? ext->backdrop_filter : nullptr; }
    void set_backdrop_filter_prop(FilterProp* value) { if (value || ext) ensure_ext()->backdrop_filter = lam::view_prop(value); }
    CssTransitionElemState* transition_state_prop() const { return ext ? ext->transition_state : nullptr; }
    void set_transition_state_prop(CssTransitionElemState* value) {
        if (value || ext) ensure_ext()->transition_state = lam::own(value);
    }
    CustomLayoutPaintState* custom_layout_paint_prop() const { return ext ? ext->custom_layout_paint : nullptr; }
    void set_custom_layout_paint_prop(CustomLayoutPaintState* value) { if (value || ext) ensure_ext()->custom_layout_paint = lam::up(value); }
    LayoutFragmentBox* layout_fragment_list() const { return ext ? ext->layout_fragments : nullptr; }
    void set_layout_fragment_list(LayoutFragmentBox* value) { if (value || ext) ensure_ext()->layout_fragments = lam::view_prop(value); }
    int layout_fragments_count() const { return ext ? ext->layout_fragment_count : 0; }
    int& layout_fragments_count_ref() { return ensure_ext()->layout_fragment_count; }
    bool has_last_remembered_width() const { return ext && ext->has_last_remembered_width; }
    bool has_last_remembered_height() const { return ext && ext->has_last_remembered_height; }
    float last_remembered_width() const { return ext ? ext->last_remembered_width : 0.0f; }
    float last_remembered_height() const { return ext ? ext->last_remembered_height : 0.0f; }
    void set_last_remembered_width(float value) {
        DomStyleRareExt* data = ensure_ext();
        if (data) {
            data->last_remembered_width = value;
            data->has_last_remembered_width = true;
        }
    }
    void set_last_remembered_height(float value) {
        DomStyleRareExt* data = ensure_ext();
        if (data) {
            data->last_remembered_height = value;
            data->has_last_remembered_height = true;
        }
    }
    CssWebAnimationState* web_animation_state() const { return ext ? ext->web_animation_state : nullptr; }
    CssCustomProp* css_variables() const { return ext ? ext->css_variables.get() : nullptr; }
    CssCustomProp* prepend_css_variable(const char* name);
    bool display_list_item() const { return display.inner.list_item(); }
    void set_display_list_item(bool value) { display.inner.set_list_item(value); }
    void set_web_animation_state(CssWebAnimationState* value) {
        if (value || ext) ensure_ext()->web_animation_state = lam::own(value);
    }
    bool has_animated_display() const { return ext && ext->has_animated_display; }
    DisplayValue animated_display() const {
        return ext ? ext->animated_display : DisplayValue{CSS_VALUE__UNDEF, CSS_VALUE__UNDEF};
    }
    void set_animated_display(DisplayValue value) {
        DomStyleRareExt* data = ensure_ext();
        if (data) {
            data->animated_display = value;
            data->has_animated_display = true;
        }
    }
    void clear_animated_display() {
        if (ext) ext->has_animated_display = false;
    }
    ShadowStyle* shadow_style() const { return ext ? ext->shadow.get() : nullptr; }
    ShadowStyle* ensure_shadow_style() { return ensure_ext_group<ShadowStyle>(&DomStyleRareExt::shadow); }
    DomElement* shadow_host_element() const {
        const ShadowStyle* state = shadow_style();
        return state ? state->host.get() : nullptr;
    }
    DomElement* shadow_root_element() const {
        const ShadowStyle* state = shadow_style();
        return state ? state->root.get() : nullptr;
    }
    bool is_closed_shadow_root() const {
        const ShadowStyle* state = shadow_style();
        return state && state->closed;
    }
    void set_shadow_host_element(DomElement* value) {
        ShadowStyle* state = value ? ensure_shadow_style() : shadow_style();
        if (state) state->host = lam::up(value);
    }
    void set_shadow_root_element(DomElement* value) {
        ShadowStyle* state = value ? ensure_shadow_style() : shadow_style();
        if (state) state->root = lam::own(value);
    }
    float pending_scroll_x() const { return ext ? ext->pending_element_scroll_x : 0.0f; }
    void set_pending_scroll_x(float value) { ensure_ext()->pending_element_scroll_x = value; }
    float pending_scroll_y() const { return ext ? ext->pending_element_scroll_y : 0.0f; }
    void set_pending_scroll_y(float value) { ensure_ext()->pending_element_scroll_y = value; }
    const char* tag_name() const {
        if (flag(ELMT_FLAG_TAG_NAME_NONCANONICAL)) return noncanonical_tag_name();
        return tag_id == NAME_ID_NONE ? nullptr : well_known_name_view(tag_id).str;
    }
    const char* noncanonical_tag_name() const;
    NameId tag_name_id() const {
        if (ext && ext->name_id != NAME_ID_NONE) return ext->name_id;
        if (flag(ELMT_FLAG_TAG_NAME_NONCANONICAL)) {
            const char* name = noncanonical_tag_name();
            return name ? well_known_name_id({name, strlen(name)}) : NAME_ID_NONE;
        }
        return tag_id;
    }
    PopoverStyle* popover_style() const { return ext ? ext->popover.get() : nullptr; }
    PopoverStyle* ensure_popover_style() { return ensure_ext_group<PopoverStyle>(&DomStyleRareExt::popover); }
    bool is_popover_open() const { const auto* data = popover_style(); return data && data->open; }
    bool is_in_top_layer() const { const auto* data = popover_style(); return data && data->in_top_layer; }
    void set_popover_open(bool value) {
        PopoverStyle* data = value ? ensure_popover_style() : popover_style();
        if (data) data->open = value;
    }
    bool is_dialog_modal() const { return ext && ext->dialog_modal; }
    void set_dialog_modal(bool value) {
        DomStyleRareExt* data = ensure_ext();
        if (data) data->dialog_modal = value;
    }
    void reset_view_ext() {
        if (!ext) return;
        // Doc-pooled storage outlives view results, so every relayout must drop
        // geometry and paint produced by the previous view-pool epoch.
        if (ext->fragment_geometry) *ext->fragment_geometry.get() = {};
        ext->layout_fragments = nullptr;
        ext->layout_fragment_count = 0;
        ext->custom_layout_paint = nullptr;
    }

};

LAM_NODE_OF(DomElement, NodeDocument);
LAM_NODE_OF(DomStyleRareExt, NodeDocument);
LAM_NODE_OF(DomDocument, NodeDocument);

inline const DomNode* dom_shadow_including_parent(const DomNode* node) {
    if (node->parent) return node->parent;
    const DomElement* element = node->as_element();
    return element ? element->shadow_host_element() : nullptr;
}

inline DomNode* dom_shadow_including_parent(DomNode* node) {
    return const_cast<DomNode*>(dom_shadow_including_parent(static_cast<const DomNode*>(node)));
}

inline bool dom_shadow_including_contains(const DomNode* root, const DomNode* node) {
    for (; node; node = dom_shadow_including_parent(node)) {
        if (node == root) return true;
    }
    return false;
}

// A shadow tree is connected when its host is connected.
inline bool dom_node_is_connected(const DomNode* node) {
    DomDocument* document = dom_node_ancestor_document(const_cast<DomNode*>(node));
    return document && dom_shadow_including_contains(document->root, node);
}

inline bool dom_element_is_connected(const DomElement* element) {
    return dom_node_is_connected(element);
}

// Generated layout nodes share the live sibling chain but are not DOM element
// children for CSS structural pseudo-classes.
static inline bool dom_is_css_element_child(const DomNode* node) {
    return node && node->is_element() && !node->as_element()->is_synthetic();
}

// The body is the document-wide editing host for designMode. Keep this DOM
// tree query below DomElement's definition so core editing code does not need
// to link against Radiant's event dispatcher just to resolve the host.
inline DomElement* dom_document_body_element(DomDocument* document) {
    DomElement* root = document ? document->root : nullptr;
    if (!root) return nullptr;
    if (root->tag_name() && strcmp(root->tag_name(), "body") == 0) return root;
    for (DomNode* child = root->first_child; child; child = child->next_sibling) {
        if (!child->is_element()) continue;
        DomElement* element = child->as_element();
        if (element && element->tag_name() && strcmp(element->tag_name(), "body") == 0) {
            return element;
        }
    }
    return nullptr;
}

// Live selectedness belongs to the DOM node, not to a particular scripting
// guest.  Rendering and every guest must observe the same native state.
inline bool dom_option_is_selected(DomElement* option) {
    if (!option) return false;
    if (option->has_option_selectedness()) return option->option_selectedness();
    return option->has_attribute("selected");
}

// Return an option's text label with HTML ASCII whitespace collapsed and trimmed.
void dom_option_text_normalized(DomElement* option, StrBuf* out);

// ============================================================================
// DomElement ↔ Element conversion (Phase 1: Unified DOM Tree)
// ============================================================================

// DomElement remains non-standard-layout because both its base and derived class
// contain storage. __builtin_offsetof is stable on the supported compilers.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

// DomElement* → Element*: returns pointer to the embedded Element within DomElement
inline Element* dom_element_to_element(DomElement* de) {
    if (de->node_flags & DOM_NODE_FLAG_GC_BACKING) {
        dom_node_registry_refresh_backing(de->doc, de);
    }
    return &de->elmt;
}
inline const Element* dom_element_to_element(const DomElement* de) {
    return dom_element_to_element(const_cast<DomElement*>(de));
}

// Synthetic layout-only nodes deliberately have no Lambda-tree identity even
// though they carry the same embedded storage for a uniform object layout.
inline Element* dom_element_backing(DomElement* de) {
    return de && !de->is_synthetic() ? dom_element_to_element(de) : nullptr;
}
inline const Element* dom_element_backing(const DomElement* de) {
    return de && !de->is_synthetic() ? dom_element_to_element(de) : nullptr;
}

// Element* → DomElement*: reverse conversion (caller must ensure Element is embedded in a DomElement)
inline DomElement* element_to_dom_element(Element* e) {
    return (DomElement*)((char*)e - offsetof(DomElement, elmt));
}
inline const DomElement* element_to_dom_element(const Element* e) {
    return (const DomElement*)((const char*)e - offsetof(DomElement, elmt));
}

// Template render-map entries are keyed by the original Lambda element, not
// a DOM wrapper's copied embedded value. The lifecycle registry preserves
// that identity across wrapper rebuilds and is therefore authoritative for
// event/template reverse lookup.
inline Element* dom_element_render_source(DomElement* de) {
    if (!de || de->is_synthetic()) return nullptr;
    Element* source = de->doc
        ? dom_node_registry_backing_source(de->doc, static_cast<DomNode*>(de))
        : nullptr;
    return source ? source : dom_element_to_element(de);
}
inline const Element* dom_element_render_source(const DomElement* de) {
    return dom_element_render_source(const_cast<DomElement*>(de));
}

// equal template results retain live DOM storage while adopting the new result keys.
void dom_rebind_equal_render_result(DomDocument* doc, Item previous, Item current);
void dom_rebind_render_element(DomDocument* doc, Element* previous, Element* current);
// data/ARIA, or an inert attribute whose boolean presence is unchanged; CSS dependencies remain caller-owned.
bool dom_element_can_write_metadata(DomElement* element, const char* name);
bool dom_element_set_non_style_metadata(DomElement* element, const char* name, const char* value);
DomElement* element_dom_map_lookup(struct hashmap* map, Element* elem);

// Resolve a parsed/template Element to its live DOM wrapper. The lifecycle
// registry preserves source identity even when a DomElement owns a copied one.
inline DomElement* dom_find_element_for_source(DomElement* root,
                                               const Element* source) {
    if (!root || !source) return nullptr;
    if (dom_element_to_element(root) == source || dom_element_render_source(root) == source) return root;
    for (DomNode* child = root->first_child; child; child = child->next_sibling) {
        if (!child->is_element()) continue;
        DomElement* found = dom_find_element_for_source(child->as_element(), source);
        if (found) return found;
    }
    return dom_find_element_for_source(root->shadow_root_element(), source);
}

// the parser-only iterator merges the detached root into its original source
// position for resource discovery. Ordinary Mark and DOM readers see light children.
struct DomParsedChildren {
    Element* parent;
    Element* shadow;
    int64_t position;
    int64_t length() const { return parent->length + (shadow ? 1 : 0); }
    Item at(int64_t index) const {
        if (!shadow || index < position) return parent->items[index];
        return index == position ? Item{.element = shadow} : parent->items[index - 1];
    }
};
DomParsedChildren dom_parsed_children(DomDocument* document, Element* parent);

// DomElement* ↔ DomNode*: same address (DomNode is at offset 0 via inheritance)
inline DomNode* dom_element_to_node(DomElement* de) { return static_cast<DomNode*>(de); }
inline DomElement* node_to_dom_element(DomNode* dn) { return static_cast<DomElement*>(dn); }

inline void dom_element_retain_id(DomElement* element, lam::PoolPtr<const char> id) {
    element->id = lam::up(id.get());
}

inline void dom_element_retain_id(DomElement* element, lam::PoolPtr<char> id) {
    dom_element_retain_id(element, lam::borrow_const(id));
}

inline void dom_element_clear_id(DomElement* element) {
    element->id = nullptr;
}

// Ensure embedded Element is 8-byte aligned (required for pointer fields in Element)
static_assert(offsetof(DomElement, elmt) % 8 == 0,
              "Embedded Element must be 8-byte aligned within DomElement");

#pragma GCC diagnostic pop

// Pseudo-class state flags
#define PSEUDO_STATE_HOVER          (1 << 0)
#define PSEUDO_STATE_ACTIVE         (1 << 1)
#define PSEUDO_STATE_FOCUS          (1 << 2)
#define PSEUDO_STATE_VISITED        (1 << 3)
#define PSEUDO_STATE_LINK           (1 << 4)
#define PSEUDO_STATE_ENABLED        (1 << 5)
#define PSEUDO_STATE_DISABLED       (1 << 6)
#define PSEUDO_STATE_CHECKED        (1 << 7)
#define PSEUDO_STATE_INDETERMINATE  (1 << 8)
#define PSEUDO_STATE_VALID          (1 << 9)
#define PSEUDO_STATE_INVALID        (1 << 10)
#define PSEUDO_STATE_REQUIRED       (1 << 11)
#define PSEUDO_STATE_OPTIONAL       (1 << 12)
#define PSEUDO_STATE_READ_ONLY      (1 << 13)
#define PSEUDO_STATE_READ_WRITE     (1 << 14)
#define PSEUDO_STATE_FIRST_CHILD    (1 << 15)
#define PSEUDO_STATE_LAST_CHILD     (1 << 16)
#define PSEUDO_STATE_ONLY_CHILD     (1 << 17)
#define PSEUDO_STATE_FOCUS_VISIBLE  (1 << 18)  // keyboard focus (Tab/arrow keys)
#define PSEUDO_STATE_FOCUS_WITHIN   (1 << 19)  // has focused descendant
#define PSEUDO_STATE_SELECTED       (1 << 20)  // option/selection state
#define PSEUDO_STATE_TARGET         (1 << 21)  // URL fragment target
#define PSEUDO_STATE_PLACEHOLDER_SHOWN (1 << 22)  // input showing placeholder
#define PSEUDO_STATE_DRAG           (1 << 23)  // element being dragged
#define PSEUDO_STATE_DRAG_OVER      (1 << 24)  // element is a drag-over target
#define PSEUDO_STATE_OPEN           (1 << 25)  // open disclosure or picker
#define PSEUDO_STATE_VALUE_DEPENDENT (1u << 26) // stylesheet dependency query for live values
#define PSEUDO_STATE_DIRECTION_DEPENDENT (1u << 27) // host direction can depend on descendant state

// ============================================================================
// DOM Document Creation and Destruction
// ============================================================================

/**
 * Create a new DomDocument
 * @param input Lambda Input context (required for MarkEditor operations)
 * @return New DomDocument or NULL on failure
 */
DomDocument* dom_document_create(Input* input);

// Adopt a replacement URL while preserving the current carrier address for
// Input and loader aliases that remain valid for the document lifetime.
bool dom_document_replace_url(DomDocument* document, Url* replacement);

// returns an owned resource base; the document URL keeps its original identity.
Url* dom_document_resolve_url(DomDocument* document);
// returns an owned URL; a null reference requests the first applicable HTML base URL.
Url* dom_document_resolve_url(const DomDocument* document, const char* reference = nullptr);

/**
 * Destroy a DomDocument and all its nodes
 * @param document Document to destroy
 */
void dom_document_destroy(DomDocument* document);

// Transfer the Input cleanup right from its owning document. Borrowing
// documents return null because their creator retains the Input context.
Input* dom_document_take_owned_input_resources(DomDocument* document);

// Mark a document as borrowing its Input from another document.
void dom_document_borrow_input_resources(DomDocument* document);

// Complete a loader-pool handoff. A replacement document can already own an
// independent pool, in which case this releases the superseded loader pool.
bool dom_document_finalize_loader_pool(DomDocument* document, Pool* pool);

// ============================================================================
// DOM Element Creation and Destruction
// ============================================================================

/**
 * JS/Jube bridge shim for DomElement::create().
 * @param doc Parent document (provides arena and input)
 * @param tag_name Element tag name (e.g., "div", "span")
 * @param native_element Pointer to backing Lambda Element; null creates a synthetic node
 * @return New DomElement or NULL on failure
 */
DomElement* dom_element_create(DomDocument* doc, const char* tag_name, Element* native_element);
// Create a detached element and its input-owned Mark backing.
DomElement* dom_create_backed_element_identity(DomDocument* document, const char* tag,
                                               const char* namespace_uri, bool qualified);
// A DOM-only or already-retired child has no Mark slot; -1 does not mean it is unlinked.
int64_t dom_backed_node_index(DomElement* parent, DomNode* child);
int64_t dom_backed_insertion_index(DomElement* parent, DomNode* reference);
// Backing-only operations: callers prepare the authored parent and publish mutation effects.
bool dom_remove_backed_element_item(DomElement* parent, DomElement* child, int64_t child_index);
bool dom_remove_prepared_backed_child(DomElement* parent, DomNode* child);
bool dom_insert_prepared_backed_text(DomElement* parent, DomText* text, DomNode* reference);
bool dom_insert_detached_backed_element(DomElement* parent, DomElement* child, DomNode* reference);
// detached, unbacked fragment nodes share the document lifetime (D5.5.1).
extern "C" DomElement* dom_document_fragment_create(DomDocument* doc);
DomElement* build_dom_tree_from_element(Element* element, DomDocument* document, DomElement* parent);

/**
 * Destroy a DomElement
 * @param element Element to destroy
 */
void dom_element_destroy(DomElement* element);
void dom_element_release_retired_storage(DomElement* element);

/**
 * Clear all data from a DomElement (without freeing the structure)
 * @param element Element to clear
 */
void dom_element_clear(DomElement* element);
void dom_element_clear_cascaded_styles(DomElement* element);
void dom_element_borrow_specified_style(DomElement* element, StyleTree* style);

// ============================================================================
// Inline Style Support
// ============================================================================

/**
 * Parse and apply inline style attribute to an element
 * @param element Target element
 * @param style_text Inline style text (e.g., "color: red; font-size: 14px")
 * @return Number of declarations applied
 */
int dom_element_apply_inline_style(DomElement* element, const char* style_text);
CssRule* dom_element_inline_declaration_block(DomElement* element);
bool dom_element_commit_inline_declarations(DomElement* element, CssRule* rule, const char* text);
// Node-owned transient CSS values, independent of the authored Mark attributes.
bool dom_element_set_presentation_style(DomElement* element, const char* property,
                                        const char* value, bool* changed);
bool dom_element_clear_presentation_style(DomElement* element);
bool dom_element_set_animation_custom_property(DomElement* element,
    const CssDeclaration* sample, const void* effect, bool* changed);
bool dom_element_clear_animation_custom_properties(DomElement* element, const void* effect,
    const char* name = nullptr);
const char* dom_inline_style_declaration_end(const char* text);

/**
 * Get inline style text from an element
 * @param element Source element
 * @return Inline style text or NULL if none
 */
const char* dom_element_get_inline_style(DomElement* element);

/**
 * Remove inline styles from an element
 * @param element Target element
 * @return true if inline styles were removed, false otherwise
 */
bool dom_element_remove_inline_styles(DomElement* element);

// ============================================================================
// Style Management
// ============================================================================

/**
 * Apply a CSS declaration to an element
 * @param element Target element
 * @param declaration CSS declaration to apply
 * @return true on success, false on failure
 */
bool dom_element_apply_declaration(DomElement* element, CssDeclaration* declaration);

// Timing functions for profiling CSS cascade
void reset_dom_element_timing();
void log_dom_element_timing();

/**
 * Apply a CSS rule to an element
 * @param element Target element
 * @param rule CSS rule with declarations
 * @param specificity Selector specificity for cascade resolution
 * @return Number of declarations applied
 */
int dom_element_apply_rule(DomElement* element, CssRule* rule, CssSpecificity specificity,
                            uint32_t scope_proximity = 0);

/**
 * Apply a CSS rule to a pseudo-element (::before or ::after)
 * @param element Target element (the originating element)
 * @param rule CSS rule with declarations
 * @param specificity Selector specificity for cascade resolution
 * @param pseudo_element Which pseudo-element (PSEUDO_ELEMENT_BEFORE or PSEUDO_ELEMENT_AFTER)
 * @return Number of declarations applied
 */
int dom_element_apply_pseudo_element_rule(DomElement* element, CssRule* rule,
                                          CssSpecificity specificity, int pseudo_element,
                                          uint32_t scope_proximity = 0, const CssSelector* selector = nullptr);

/**
 * Get the specified value for a CSS property
 * @param element Target element
 * @param property_code Property to look up
 * @return Specified CSS declaration or NULL if not set
 */
CssDeclaration* dom_element_get_specified_value(DomElement* element, CssPropertyCode property_code);

/**
 * Get the specified value for a CSS property on a pseudo-element
 * @param element Target element (the originating element)
 * @param property_code Property to look up
 * @param pseudo_element Which pseudo-element (PSEUDO_ELEMENT_BEFORE or PSEUDO_ELEMENT_AFTER)
 * @return Specified CSS declaration or NULL if not set
 */
StyleTree* dom_element_get_pseudo_style(DomElement* element, int pseudo_element);
CssDeclaration* dom_element_get_pseudo_element_value(DomElement* element,
                                                     CssPropertyCode property_code, int pseudo_element);

/**
 * Check if element has ::before pseudo-element content
 * @param element Target element
 * @return true if element has ::before content, false otherwise
 */
bool dom_element_has_before_content(DomElement* element);

/**
 * Check if element has ::after pseudo-element content
 * @param element Target element
 * @return true if element has ::after content, false otherwise
 */
bool dom_element_has_after_content(DomElement* element);

/**
 * Get content string for a pseudo-element
 * @param element Target element
 * @param pseudo_element Which pseudo-element (PSEUDO_ELEMENT_BEFORE or PSEUDO_ELEMENT_AFTER)
 * @return Content string or NULL if none
 */
const char* dom_element_get_pseudo_element_content(DomElement* element, int pseudo_element);
struct StrBuf;
// a layout mode may resolve content functions the DOM evaluator does not know
// (target-counter(), content(), page counters); it sets `handled` for those
typedef bool (*DomContentFunctionHook)(void* context, DomElement* element,
    const CssFunction* function, StrBuf* text, bool* handled);
bool dom_element_append_content(DomElement* element, const CssValue* value,
    void* counter_context, int* quote_depth, StrBuf* text,
    DomContentFunctionHook hook = nullptr, void* hook_context = nullptr);

/**
 * Get pseudo-element content with counter resolution
 * Extended version that handles counter() and counters() functions
 * @param counter_context Pointer to CounterContext (from radiant/layout_counters.hpp)
 * @param arena Arena for allocating result string
 */
const char* dom_element_get_pseudo_element_content_with_counters(
    DomElement* element, int pseudo_element, void* counter_context, Arena* arena,
    int* quote_depth = nullptr, DomContentFunctionHook hook = nullptr, void* hook_context = nullptr);

/**
 * Remove a CSS property from an element
 * @param element Target element
 * @param property_code Property to remove
 * @return true if property was removed, false if not found
 */
bool dom_element_remove_property(DomElement* element, CssPropertyCode property_code);

/**
 * Clear every stylesheet-derived pseudo-element style tree before recascade.
 * Returns true when at least one pseudo style tree was cleared.
 */
bool dom_element_clear_pseudo_styles(DomElement* element);

/** Return whether target is root or one of root's DOM descendants. */
bool dom_subtree_contains_node(DomNode* root, DomNode* target);

/** Replace a linked DOM child without rebuilding the surrounding sibling chain. */
bool dom_node_replace_in_parent(DomElement* parent, DomNode* old_child,
                                DomNode* new_child);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get element statistics
 * @param element Element to analyze
 * @param specified_count Output: number of specified properties
 * @param computed_count Output: number of computed properties
 * @param total_declarations Output: total number of declarations in cascade
 */
void dom_element_get_style_stats(DomElement* element, int* specified_count,
    int* computed_count, int* total_declarations);

/**
 * Clone a DomElement (deep copy)
 * @param source Element to clone
 * @param pool Memory pool for new element
 * @return Cloned element or NULL on failure
 */
DomElement* dom_element_clone(DomElement* source, Pool* pool);

DomElement* dom_parent_element(DomElement* element);

inline bool dom_is_generated_pseudo_node(DomNode* node) {
    if (!node || !node->is_element()) return false;
    DomElement* element = node->as_element();
    return element->tag_name() && element->tag_name()[0] == ':' && element->tag_name()[1] == ':';
}

#endif // DOM_ELEMENT_H

// shared DOM/SVG adapter from lexical XML QNames to Mark fields.
const char* extract_element_attribute(Element* element, const char* name, Arena* arena);
