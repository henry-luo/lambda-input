#include "dom_lifecycle.hpp"

#include "dom_element.hpp"
#include "dom_node.hpp"
#include "../../core/data.hpp"
#include "lib/arena.h"
#include "lib/log.h"
#include "lib/time_util.h"
#include "lib/mem_grow.hpp"
#include "lib/memtrack.h"
#include "lib/hashmap_typed.hpp"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint64_t dom_node_ref_identity_hash(DomNodeRef ref, uint64_t seed0, uint64_t seed1) {
    return hashmap_hash_bytes(&ref.expected_id, sizeof(ref.expected_id), seed0, seed1);
}

int dom_node_ref_identity_compare(DomNodeRef left, DomNodeRef right) {
    return left.expected_id < right.expected_id ? -1 : left.expected_id > right.expected_id;
}

struct DomPinDiagnostic {
    DomNodeRef ref;
    DomNodePinReason reason;
    uint32_t count;
    DomPinDiagnostic* next;
};

struct DomRecycleWork {
    DomRecycleWork* next;
    Arena* arena;
    size_t size;
};

// only roots awaiting disposal own pending work. Children are reached structurally.
struct DomDetachedRoot {
    DomNode* node;
    size_t stranded_bytes;
    DomDetachedRoot* next;
    DomDetachedRoot* prev;
};

// only GC-backed occurrences need a stable root slot and compaction fixups.
struct DomManagedBacking {
    DomNode* node;
    void* heap;
    Item value;
    void* borrowed_data;
    Item* borrowed_items;
    bool borrows_data;
    bool borrows_items;
    DomManagedBacking* next;
};

typedef struct DomNodeRegistry {
    DomManagedBacking* managed_roots;
    DomPinDiagnostic* pin_diagnostics;
    struct DomNodeRegistry* next_observed;
    DomNode** roots; // unpublished and detached ownership roots
    size_t root_count, root_capacity;
    DomNode* inline_roots[8];
    uint32_t lifetime_stamp;
    DomNode* recent[32]; // bounded weak lookup cache, evicted before storage is released
    DomNodeValidationScope* validation_scope;

    DomDetachedRoot* candidates;
    DomDetachedRoot* next_candidate; // cancellations may advance the active traversal
    bool collecting;
    DomDocument* document;
    DomRecycleWork* pending_free;
    struct DomNodeRegistry* queue_prev;
    struct DomNodeRegistry* queue_next;
    bool queued;
    bool sweep_requested;
    bool destroying;
    size_t stranded_after_collection;  // wrapper-stranded level after the host's last collection
    DomLifecycleStats stats;
} DomNodeRegistry;

static thread_local bool dom_retirement_deferred;
static thread_local DomNodeRegistry* dom_retirement_queue;
static thread_local DomNodeRegistry* dom_retirement_queue_tail;

// Only explicitly observed documents join this diagnostic list. A releasing
// handle may outlive its issuer, so its old document address is just a key.
static thread_local DomNodeRegistry* dom_observed_documents;

static DomNodeRegistry** dom_observer_slot(DomDocument* issuer) {
    auto** slot = &dom_observed_documents;
    while (*slot && (*slot)->document != issuer) slot = &(*slot)->next_observed;
    return slot;
}

// A stamp proves that no node has lost its address since a live reference was
// minted. On exhaustion every subsequent document uses structural validation.
static uint32_t dom_next_lifetime_stamp() {
    static uint32_t sequence;
    uint32_t last = __atomic_load_n(&sequence, __ATOMIC_RELAXED);
    while (last != UINT32_MAX) {
        if (__atomic_compare_exchange_n(&sequence, &last, last + 1, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return last + 1;
    }
    return 0;
}

static void dom_retire_dequeue(DomNodeRegistry* registry) {
    if (!registry || !registry->queued) return;
    if (registry->queue_prev) registry->queue_prev->queue_next = registry->queue_next;
    else dom_retirement_queue = registry->queue_next;
    if (registry->queue_next) registry->queue_next->queue_prev = registry->queue_prev;
    else dom_retirement_queue_tail = registry->queue_prev;
    registry->queue_prev = registry->queue_next = nullptr;
    registry->queued = false;
}

static void dom_retire_enqueue(DomNodeRegistry* registry) {
    if (registry->queued || registry->destroying) return;
    registry->queue_prev = dom_retirement_queue_tail;
    if (dom_retirement_queue_tail) dom_retirement_queue_tail->queue_next = registry;
    else dom_retirement_queue = registry;
    dom_retirement_queue_tail = registry;
    registry->queued = true;
}

// DOM-only unit targets do not link the Radiant view teardown implementation.
__attribute__((weak)) void view_tree_release_retired_subtree(ViewTree*, DomNode*) {}
__attribute__((weak)) void view_tree_release_detached_embedded_documents(ViewTree*, DomNode*) {}
__attribute__((weak)) void view_tree_prepare_detached_subtree(DomNode*) {}
__attribute__((weak)) void view_pool_release_detached_form_props(DomNode*) {}
__attribute__((weak)) uint32_t view_state_detach_subtree(DocState*, DomNode*) { return 0; }
__attribute__((weak)) void form_control_adopt_document(DomElement*) {}
__attribute__((weak)) void form_control_release_document(DomDocument*, bool) {}
__attribute__((weak)) void dom_retire_release_render_result(DomDocument*, Item) {}
extern "C" __attribute__((weak)) void* dom_backing_gc_owner(DomDocument*, Item, void*) { return nullptr; }
extern "C" __attribute__((weak)) void dom_retain_backing_root(void*, Item*) {}
extern "C" __attribute__((weak)) void dom_release_backing_root(void*, Item*) {}
extern "C" __attribute__((weak)) bool dom_wrapper_adopt_document(
    DomDocument*, DomDocument*, DomNode*, bool) { return true; }
extern "C" __attribute__((weak)) uint32_t dom_wrapper_hold_count(DomDocument*, DomNode*) { return 0; }
extern "C" __attribute__((weak)) void dom_cssom_invalidate_document(void*) {}
extern "C" __attribute__((weak)) void dom_expando_attachment_changed(
    DomDocument*, DomNode*, bool) {}

static void dom_lifecycle_fail(const char* operation, DomNodeRef ref,
                               DomNodePinReason reason) {
    log_error("DOM_LIFECYCLE_INVARIANT: op=%s address=%p expected_id=%u reason=%u",
              operation ? operation : "unknown", (void*)ref.address,
              ref.expected_id, (unsigned)reason);
#ifndef NDEBUG
    assert(false && "DOM lifecycle registry invariant failed");
#else
    abort();
#endif
}

static DomNodeRegistry* dom_registry(DomDocument* doc) {
    return doc ? (DomNodeRegistry*)doc->services.node_registry : nullptr;
}

static DomNode*& dom_cached_address(DomNodeRegistry* registry, DomNode* address) {
    uintptr_t value = (uintptr_t)address;
    return registry->recent[((value >> 4) ^ (value >> 10)) % 32];
}

static void dom_forget_address(DomNodeRegistry* registry, DomNode* node) {
    DomNode*& cached = dom_cached_address(registry, node);
    if (cached == node) cached = nullptr;
}

static bool dom_reserve_roots(DomNodeRegistry* registry, size_t count) {
    if (count <= registry->root_capacity) return true;
    // inline storage belongs to the service and must never be passed to pool_free.
    DomNode** roots = registry->roots == registry->inline_roots ? nullptr : registry->roots;
    size_t capacity = roots ? registry->root_capacity : 0;
    if (!lam::pool_grow_array(registry->document->document_pool, &roots, &capacity, count, 16))
        return false;
    if (registry->roots == registry->inline_roots)
        memcpy(roots, registry->inline_roots, registry->root_count * sizeof(*roots));
    registry->roots = roots;
    registry->root_capacity = capacity;
    return true;
}

static bool dom_own_root(DomNodeRegistry* registry, DomNode* node) {
    if (node->detached_root_slot) return true;
    if (registry->root_count == UINT32_MAX || !dom_reserve_roots(registry, registry->root_count + 1)) return false;
    registry->roots[registry->root_count++] = node;
    node->detached_root_slot = (uint32_t)registry->root_count;
    return true;
}

void dom_node_own_unlinked(DomNode* node) {
    if (node->lifecycle_live && !dom_own_root(dom_registry(node->doc), node))
        dom_lifecycle_fail("own-unlinked-root", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
}

static void dom_disown_root(DomNodeRegistry* registry, DomNode* node) {
    if (!node->detached_root_slot) return;
    size_t index = node->detached_root_slot - 1;
    DomNode* last = registry->roots[--registry->root_count];
    registry->roots[index] = last;
    last->detached_root_slot = node->detached_root_slot;
    node->detached_root_slot = 0;
    if (registry->roots != registry->inline_roots && registry->root_count <= 8) {
        memcpy(registry->inline_roots, registry->roots, registry->root_count * sizeof(last));
        pool_free(registry->document->document_pool, registry->roots);
        registry->roots = registry->inline_roots;
        registry->root_capacity = 8;
    }
}

// Attributes, shadow roots and template contents have separate ownership edges.
template <typename Inspect>
static bool dom_visit_owned_tree(DomNode* root, const Inspect& inspect) {
    if (inspect(root)) return true;
    DomElement* element = root->as_element();
    if (!element) return false;
    if (element->rare_ext) {
        for (DomAttr* attr = element->rare_ext->attribute_nodes; attr; attr = attr->attribute_next)
            if (inspect(attr)) return true;
        DomElement* shadow = element->shadow_root_element();
        if (shadow && dom_visit_owned_tree(shadow, inspect)) return true;
        DomElement* content = element->template_contents();
        if (content && dom_visit_owned_tree(content, inspect)) return true;
    }
    for (DomNode* child = element->first_child; child; child = child->next_sibling)
        if (dom_visit_owned_tree(child, inspect)) return true;
    return false;
}

template <typename Inspect>
static bool dom_visit_owned_nodes(DomNodeRegistry* registry, const Inspect& inspect) {
    DomNode* published = registry->document->root;
    if (published && dom_visit_owned_tree(published, inspect)) return true;
    for (size_t i = registry->root_count; i; --i) {
        DomNode* root = registry->roots[i - 1];
        if (root != published && dom_visit_owned_tree(root, inspect)) return true;
    }
    return false;
}

static bool dom_probe_owned_address(DomNodeRegistry* registry, DomNode* node) {
    // compare a weak address only against known live storage before reading it.
    bool found = dom_visit_owned_nodes(registry, [node](DomNode* member) { return member == node; });
    return found && node->lifecycle_live && node->doc == registry->document;
}

bool dom_node_registry_owns(DomDocument* doc, DomNode* node) {
    auto* registry = dom_registry(doc);
    if (!registry || !node || registry->destroying) return false;
    DomNode*& cached = dom_cached_address(registry, node);
    if (cached == node) return true;
    bool found = registry->validation_scope ? registry->validation_scope->owns_address(node)
                                            : dom_probe_owned_address(registry, node);
    if (found) cached = node;
    return found;
}

struct DomValidationAddress { DomNode* node; };
using DomValidationSet = TypedHashMap<DomValidationAddress,
    HashMapPointerMemberKeyOps<DomValidationAddress, &DomValidationAddress::node>>;

DomNodeValidationScope::DomNodeValidationScope(DomDocument* doc) {
    if (auto* registry = dom_registry(doc); registry && !registry->validation_scope) {
        registry->validation_scope = this;
        document = doc;
    }
}

DomNodeValidationScope::~DomNodeValidationScope() {
    if (auto* registry = dom_registry(document)) registry->validation_scope = nullptr;
    DomValidationSet::destroy(addresses);
}

bool DomNodeValidationScope::owns_address(DomNode* address) {
    auto* registry = dom_registry(document);
    // a retirement or transfer in progress cannot lend a stable snapshot.
    if (!registry->lifetime_stamp) return dom_probe_owned_address(registry, address);

    if (stamp != registry->lifetime_stamp || registrations != registry->stats.registered_nodes) {
        // the first cold check needs only one walk. Further checks share a snapshot.
        DomValidationSet::destroy(addresses);
        addresses = nullptr;
        indexed = false;
        stamp = registry->lifetime_stamp;
        registrations = registry->stats.registered_nodes;
        return dom_probe_owned_address(registry, address);
    }
    if (!indexed) {
        indexed = true;
        addresses = DomValidationSet::create(0);
        if (addresses && dom_visit_owned_nodes(registry, [this](DomNode* node) {
            if (node->lifecycle_live && node->doc == document) DomValidationSet::set(addresses, {node});
            return DomValidationSet::oom(addresses);
        })) {
            DomValidationSet::destroy(addresses);
            addresses = nullptr;
        }
    }
    return addresses ? DomValidationSet::get(addresses, {address}) != nullptr
                     : dom_probe_owned_address(registry, address);
}

static DomDetachedRoot* dom_find_candidate(DomNodeRegistry* registry, DomNode* node) {
    if (!(node->node_flags & DOM_NODE_FLAG_DETACHED_CANDIDATE)) return nullptr;
    for (auto* work = registry->candidates; work; work = work->next)
        if (work->node == node) return work;
    return nullptr;
}

static void dom_end_candidacy(DomNodeRegistry* registry, DomDetachedRoot* work) {
    if (!work) return;
    work->node->node_flags &= ~DOM_NODE_FLAG_DETACHED_CANDIDATE;
    if (registry->next_candidate == work) registry->next_candidate = work->next;
    if (work->prev) work->prev->next = work->next;
    else registry->candidates = work->next;
    if (work->next) work->next->prev = work->prev;
    registry->stats.wrapper_stranded_bytes -= work->stranded_bytes;
    if (registry->stranded_after_collection > registry->stats.wrapper_stranded_bytes)
        registry->stranded_after_collection = registry->stats.wrapper_stranded_bytes;
    pool_free(registry->document->document_pool, work);
}

static Arena* dom_node_primary_arena(DomDocument* doc, DomNode* node) {
    if (!doc || !node) return nullptr;
    if (doc->node_arena && arena_owns(doc->node_arena, node)) {
        return doc->node_arena;
    }
    // UI-mode Lambda values embed DomNode storage in the retained Input arena.
    // Recording that physical owner keeps the single arena recycler valid for
    // both parsed nodes and fat Lambda-backed nodes.
    if (doc->input && doc->input->arena &&
        arena_owns(doc->input->arena, node)) {
        return doc->input->arena;
    }
    return nullptr;
}



bool dom_lifecycle_init(DomDocument* doc) {
    if (!doc || !doc->document_pool) return false;
    if (doc->services.node_registry) return true;
    DomNodeRegistry* registry = (DomNodeRegistry*)pool_calloc(
        doc->document_pool, sizeof(DomNodeRegistry));
    if (!registry) return false;
    registry->document = doc;
    registry->lifetime_stamp = dom_next_lifetime_stamp();
    doc->services.node_registry = registry;
    registry->roots = registry->inline_roots;
    registry->root_capacity = 8;

    return true;
}

static DomManagedBacking** dom_managed_backing_slot(DomDocument* doc, DomNode* node) {
    DomManagedBacking** slot = &dom_registry(doc)->managed_roots;
    while (*slot && (*slot)->node != node) slot = &(*slot)->next;
    return slot;
}

static void dom_managed_backing_release(DomManagedBacking** slot) {
    DomManagedBacking* root = *slot;
    *slot = root->next;
    dom_release_backing_root(root->heap, &root->value);
    root->node->node_flags &= ~DOM_NODE_FLAG_GC_BACKING;
    mem_free(root);
}

void dom_lifecycle_release_backing_roots(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (registry) {
        while (registry->managed_roots) dom_managed_backing_release(&registry->managed_roots);
    }
}

void dom_node_set_backing_value(DomDocument* doc, DomNode* node, Item value) {
    if (!node || node->doc != doc)
        dom_lifecycle_fail("set-backing-owner", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
    DomManagedBacking* root = nullptr;
    void* previous_heap = nullptr;
    if (node->node_flags & DOM_NODE_FLAG_GC_BACKING) {
        DomManagedBacking** slot = dom_managed_backing_slot(doc, node);
        previous_heap = (*slot)->heap;
        if ((*slot)->value.item == value.item) root = *slot;
        else dom_managed_backing_release(slot);
    }
    Element* source = get_type_id(value) == LMD_TYPE_ELEMENT ? value.element : nullptr;
    if (DomElement* element = node->as_element()) {
        Element* backing = &element->elmt;
        if (source && source != backing) {
            if (!element->occurrence_source)
                element->occurrence_source = lam::own((DomOccurrenceSource*)pool_calloc(
                    element->storage_pool(), sizeof(DomOccurrenceSource)));
            if (!element->occurrence_source)
                dom_lifecycle_fail("allocate-occurrence-source", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
            auto* occurrence = element->occurrence_source.get();
            occurrence->element = source;
            occurrence->borrowed_type = source->type == backing->type ? backing->type : nullptr;
            occurrence->borrowed_data = source->data == backing->data ? backing->data : nullptr;
        } else if (element->occurrence_source && element->occurrence_source->numeric_attributes) {
            auto* occurrence = element->occurrence_source.get();
            occurrence->element = nullptr;
            occurrence->borrowed_type = nullptr;
            occurrence->borrowed_data = nullptr;
        } else {
            lam::free_owned(element->storage_pool(), element->occurrence_source);
        }
        if (source == backing) node->node_flags |= DOM_NODE_FLAG_EMBEDDED_SOURCE;
        else node->node_flags &= ~DOM_NODE_FLAG_EMBEDDED_SOURCE;
    }
    if (!root) {
        void* heap = dom_backing_gc_owner(doc, value, previous_heap);
        if (!heap) return;
        root = (DomManagedBacking*)mem_calloc(1, sizeof(DomManagedBacking), MEM_CAT_DOM);
        if (!root) dom_lifecycle_fail("allocate-precise-root", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
        root->heap = heap;
        root->node = node;
        root->value = value;
        dom_retain_backing_root(heap, &root->value);
        DomNodeRegistry* registry = dom_registry(doc);
        root->next = registry->managed_roots;
        registry->managed_roots = root;
        node->node_flags |= DOM_NODE_FLAG_GC_BACKING;
    }
    if (source && node->is_element()) {
        Element* backing = &node->as_element()->elmt;
        root->borrowed_data = backing->data;
        root->borrowed_items = backing->items;
        root->borrows_data = backing->data == source->data;
        root->borrows_items = backing->items == source->items;
    }
}

void dom_lifecycle_destroy(DomDocument* doc) {
    if (!doc) return;
    dom_retire_begin_destroy(doc);
    dom_lifecycle_release_backing_roots(doc);
    DomNodeRegistry* registry = dom_registry(doc);
    if (registry) {
        dom_retire_dequeue(registry);
        if (registry->validation_scope) registry->validation_scope->document = nullptr;
    }
    auto** observed = dom_observer_slot(doc);
    if (*observed) *observed = (*observed)->next_observed;
    // the pool owns remaining detached roots and document-scoped work.
    doc->services.node_registry = nullptr;
}

static size_t dom_node_allocation_size(const DomNode* node) {
    switch (node->node_type) {
        case DOM_NODE_ELEMENT: return sizeof(DomElement);
        case DOM_NODE_ATTRIBUTE: return sizeof(DomAttr);
        case DOM_NODE_COMMENT:
        case DOM_NODE_DOCTYPE: return sizeof(DomComment);
        case DOM_NODE_TEXT:
            // the current text can be a replacement; the original inline String retains its capacity.
            return (node->node_flags & DOM_NODE_FLAG_INLINE_STRING)
                ? sizeof(DomText) + sizeof(String) + dom_text_to_string(node->as_text())->len + 1
                : sizeof(DomText);
        default: return 0;
    }
}

bool dom_node_registry_register(DomDocument* doc, DomNode* node,
                                size_t primary_size, bool recyclable) {
    auto* registry = dom_registry(doc);
    if (!registry || !node || !node->id || !primary_size ||
        primary_size != dom_node_allocation_size(node)) return false;
    if (node->lifecycle_live) return node->doc == doc;
    Arena* arena = recyclable ? dom_node_primary_arena(doc, node) : nullptr;
    if ((recyclable && !arena) || !dom_own_root(registry, node)) return false;
    node->recycling_arena = arena;
    node->doc = lam::up(doc);
    node->lifecycle_live = true;
    dom_cached_address(registry, node) = node;
    ++registry->stats.registered_nodes;
    return true;
}

struct DomAdoptionStep {
    DomNode* node;
    uint32_t id;
};

static bool dom_prepare_adoption_step(DomDocument* source, DomDocument* destination,
        DomNode* node, uint32_t id, DomAdoptionStep* step) {
    if (!source || !dom_registry(destination) || source == destination || !node || !id ||
        !node->lifecycle_live || node->doc != source || node->preparing_adoption) return false;
    if (node->is_element() && !node->as_element()->preserve_storage_owner()) return false;
    if (node->external_retain_count && !dom_wrapper_adopt_document(source, destination, node, false)) return false;
    if (!dom_slot_prepare_adoption(source, destination, node)) return false;
    node->preparing_adoption = true;
    *step = {node, id};
    return true;
}

static void dom_finish_adoption_step(DomDocument* source, DomDocument* destination,
        DomAdoptionStep step, bool commit) {
    DomNode* node = step.node;
    node->preparing_adoption = false;
    if (!commit) {
        dom_slot_finish_adoption(source, destination, node, false);
        return;
    }
    auto* from = dom_registry(source);
    auto* to = dom_registry(destination);
    dom_node_refresh_managed_backing(source, node);
    dom_forget_address(from, node);
    dom_end_candidacy(from, dom_find_candidate(from, node));
    dom_disown_root(from, node);
    if (source->root == node) {
        source->root = nullptr;
        source->html_root = nullptr;
    }
    ++to->stats.registered_nodes;

    if (node->node_flags & DOM_NODE_FLAG_GC_BACKING) {
        // keep the slot address and the registering heap stable across adoption.
        DomManagedBacking** slot = dom_managed_backing_slot(source, node);
        DomManagedBacking* root = *slot;
        *slot = root->next;
        root->next = dom_registry(destination)->managed_roots;
        dom_registry(destination)->managed_roots = root;
    }
    node->id = step.id;
    node->doc = lam::up(destination);
    dom_cached_address(to, node) = node;

    if (node->external_retain_count) dom_wrapper_adopt_document(source, destination, node, true);
    if (node->is_element() && node->as_element()->form_storage()) form_control_adopt_document(node->as_element());
    dom_slot_finish_adoption(source, destination, node, true);
}

bool dom_node_registry_transfer(DomDocument* source, DomDocument* destination,
        DomNode* node, uint32_t* destination_id) {
    DomAdoptionStep step;
    if (!destination_id || !dom_registry(destination) ||
        !dom_reserve_roots(dom_registry(destination), dom_registry(destination)->root_count + 1) ||
        !dom_prepare_adoption_step(source, destination, node, *destination_id, &step)) return false;
    dom_registry(source)->lifetime_stamp = 0;
    dom_finish_adoption_step(source, destination, step, true);
    dom_registry(source)->lifetime_stamp = dom_next_lifetime_stamp();
    dom_own_root(dom_registry(destination), node);
    *destination_id = step.id;
    return true;
}

struct DomAdoptionBatch {
    DomDocument* source;
    DomDocument* destination;
    DomAdoptionStep* steps;
    size_t count, capacity;
};

static bool dom_prepare_adoption_subtree(DomAdoptionBatch* batch, DomNode* node) {
    if (!lam::pool_grow_array(batch->destination->document_pool, &batch->steps,
            &batch->capacity, batch->count + 1, 16)) return false;
    DomAdoptionStep& step = batch->steps[batch->count];
    if (!dom_prepare_adoption_step(batch->source, batch->destination, node,
        dom_document_alloc_node_id(batch->destination), &step)) return false;
    ++batch->count;
    DomElement* element = node->as_element();
    if (!element) return true;
    for (DomAttr* attr = element->rare_ext ? element->rare_ext->attribute_nodes.get() : nullptr;
         attr; attr = attr->attribute_next)
        if (!dom_prepare_adoption_subtree(batch, attr)) return false;
    for (DomNode* child = element->first_child; child; child = child->next_sibling)
        if (!dom_prepare_adoption_subtree(batch, child)) return false;
    DomElement* shadow = element->shadow_root_element();
    if (shadow && !dom_prepare_adoption_subtree(batch, shadow)) return false;
    DomElement* content = element->template_contents();
    return !content || dom_prepare_adoption_subtree(batch, content);
}

bool dom_adopt_detached_subtree(DomDocument* source, DomDocument* destination, DomNode* node) {
    if (!source || !destination || !node || node->parent || !dom_registry(destination)) return false;
    // Move template-owned nodes as part of the host's native ownership tree.
    if (DomElement* element = node->as_element(); element && element->template_content_host()) return false;
    DomAdoptionBatch batch = {source, destination};
    bool prepared = dom_reserve_roots(dom_registry(destination), dom_registry(destination)->root_count + 1) &&
        dom_prepare_adoption_subtree(&batch, node);
    if (prepared) dom_registry(source)->lifetime_stamp = 0;
    for (size_t i = 0; i < batch.count; ++i) {
        DomNode* member = batch.steps[i].node;
        dom_finish_adoption_step(source, destination, batch.steps[i], prepared);
        if (prepared) member->view_state_ref = nullptr;
    }
    if (prepared) {
        dom_document_preserve_embedded_storage(source);
        dom_registry(source)->lifetime_stamp = dom_next_lifetime_stamp();
        dom_own_root(dom_registry(destination), node);
        // owner callbacks can inspect other members; publish the complete closure first.
        for (size_t i = 0; i < batch.count; ++i) {
            DomNode* member = batch.steps[i].node;
            if (DomAttr* attr = member->as_attribute()) {
                if (attr->owner_changed) attr->owner_changed(attr);
            } else if (member->is_element() && member->as_element()->shadow_root_element()) {
                destination->js.has_shadow_roots = true;
            }
        }
    }
    pool_free(destination->document_pool, batch.steps);
    return prepared;
}

void dom_node_set_backing_source(DomDocument* doc, DomNode* node,
                                          Element* backing_source) {
    dom_node_set_backing_value(doc, node,
        backing_source ? Item{.element = backing_source} : ItemNull);
}

Element* dom_node_backing_source(DomDocument* doc, DomNode* node) {
    if (!node || node->doc != doc) return nullptr;
    if (DomElement* element = node->as_element()) {
        if (element->occurrence_source && element->occurrence_source->element)
            return element->occurrence_source->element;
        return (node->node_flags & DOM_NODE_FLAG_EMBEDDED_SOURCE) ? &element->elmt : nullptr;
    }
    DomComment* comment = node->as_comment();
    return comment ? comment->native_element.get() : nullptr;
}

void dom_node_borrowed_source_storage(DomDocument* doc, DomNode* node,
                                             bool* shared_type, bool* shared_data) {
    DomElement* element = node && node->doc == doc ? node->as_element() : nullptr;
    DomOccurrenceSource* source = element ? element->occurrence_source.get() : nullptr;
    if (shared_type) *shared_type = source && source->borrowed_type &&
        element->elmt.type == source->borrowed_type;
    if (shared_data) *shared_data = source && source->borrowed_data &&
        element->elmt.data == source->borrowed_data;
}

void dom_node_refresh_managed_backing(DomDocument* doc, DomNode* node) {
    if (!node || !(node->node_flags & DOM_NODE_FLAG_GC_BACKING) || !node->is_element() || !dom_registry(doc)) return;
    DomManagedBacking* root = *dom_managed_backing_slot(doc, node);
    if (!root) return;
    Element* source = root->value.element;
    Element* backing = &node->as_element()->elmt;
    // only the GC owner gets compaction fixups; pull its borrowed pointers on access.
    // an Input-owned DOM edit ends the corresponding borrow instead of being overwritten.
    if (root->borrows_data) {
        root->borrows_data = backing->data == root->borrowed_data;
        if (root->borrows_data) {
            // keep MarkEditor’s copy-on-write ownership check valid after payload relocation.
            auto* occurrence = node->as_element()->occurrence_source.get();
            if (occurrence && occurrence->borrowed_data == root->borrowed_data)
                occurrence->borrowed_data = source->data;
            backing->data = root->borrowed_data = source->data;
            backing->data_cap = source->data_cap;
        }
    }
    if (root->borrows_items) {
        root->borrows_items = backing->items == root->borrowed_items;
        if (root->borrows_items) {
            backing->items = root->borrowed_items = source->items;
        }
    }
}

DomNodeRef dom_node_ref(DomNode* node) {
    DomNodeRef ref = {node, node ? node->id : 0, 0};
    if (node && node->lifecycle_live) {
        if (auto* registry = dom_registry(node->doc)) {
            ref.lifetime_stamp = registry->lifetime_stamp;
            dom_cached_address(registry, node) = node;
        }
    }
    return ref;
}

static DomNode* dom_validate_weak_ref(DomNodeRegistry* registry, DomNodeRef ref) {
    if (!registry || !ref.address || !ref.expected_id) return nullptr;
    bool accessible = ref.lifetime_stamp && ref.lifetime_stamp == registry->lifetime_stamp;
    if ((accessible || dom_node_registry_owns(registry->document, ref.address)) && ref.address->id == ref.expected_id)
        return ref.address;
    ++registry->stats.stale_ref_rejections;
    return nullptr;
}

DomNode* dom_node_ref_validate(DomDocument* doc, DomNodeRef ref) {
    return dom_validate_weak_ref(dom_registry(doc), ref);
}

DomNode* dom_node_ref_refresh(DomDocument* doc, DomNodeRef& ref) {
    DomNodeRegistry* registry = dom_registry(doc);
    DomNode* node = dom_validate_weak_ref(registry, ref);
    if (node && ref.lifetime_stamp != registry->lifetime_stamp) ref.lifetime_stamp = registry->lifetime_stamp;
    return node;
}

// callers already own a live reference when transferring a hold (D5.5.1).
void dom_node_retain_live(DomNode* node) {
    if (!node) return;
    if (node->external_retain_count == UINT32_MAX)
        dom_lifecycle_fail("retain-overflow", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
    ++node->external_retain_count;
}

void dom_node_release_live(DomNode* node) {
    if (!node) return;
    if (!node->external_retain_count)
        dom_lifecycle_fail("release-underflow", dom_node_ref(node), DOM_NODE_PIN_EXTERNAL);
    if (--node->external_retain_count == 0) {
        DomNodeRegistry* registry = dom_registry(node->doc);
        if (registry && registry->candidates && !registry->destroying) {
            // schedule against the current owner, even when an older realm releases its handle.
            registry->sweep_requested = true;
            dom_retire_enqueue(registry);
        }
    }
}

// Verification is explicitly enabled by test fixtures before they acquire handles.
void dom_observe_native_pins(DomDocument* doc) {
    auto* registry = dom_registry(doc);
    if (!registry || *dom_observer_slot(doc)) return;
    registry->next_observed = dom_observed_documents;
    dom_observed_documents = registry;
}

static DomPinDiagnostic** dom_pin_diagnostic_slot(DomNodeRegistry* registry,
        DomNodeRef ref, DomNodePinReason reason) {
    auto** slot = &registry->pin_diagnostics;
    while (*slot && ((*slot)->ref.address != ref.address ||
           (*slot)->ref.expected_id != ref.expected_id || (*slot)->reason != reason))
        slot = &(*slot)->next;
    return slot;
}

static void dom_observe_pin_change(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason, bool acquire) {
    auto* registry = *dom_observer_slot(doc);
    if (!registry) return;
    auto** slot = dom_pin_diagnostic_slot(registry, ref, reason);
    if (acquire) {
        if (!*slot) {
            *slot = (DomPinDiagnostic*)pool_alloc(doc->document_pool, sizeof(DomPinDiagnostic));
            if (!*slot) dom_lifecycle_fail("allocate-pin-diagnostic", ref, reason);
            **slot = {ref, reason, 0, nullptr};
        }
        ++(*slot)->count;
    } else if (*slot && !--(*slot)->count) {
        auto* entry = *slot;
        *slot = entry->next;
        pool_free(doc->document_pool, entry);
    }
}

uint32_t dom_node_pin_count(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason) {
    auto* registry = *dom_observer_slot(doc);
    if (!registry) return 0;
    auto* entry = *dom_pin_diagnostic_slot(registry, ref, reason);
    return entry ? entry->count : 0;
}

bool dom_node_pin(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || reason >= DOM_NODE_PIN_REASON_COUNT) return false;
    if (!dom_node_ref_validate(doc, ref)) dom_lifecycle_fail("pin-stale", ref, reason);
    DomNode* node = ref.address;
    dom_node_retain_live(node);
    if (reason == DOM_NODE_PIN_WRAPPER) ++node->wrapper_pin_count;
    dom_observe_pin_change(doc, ref, reason, true);
    return true;
}

bool dom_node_unpin(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason) {
    if (!ref.address || reason >= DOM_NODE_PIN_REASON_COUNT) return false;
    // the holder guarantees addressability until this release, including during teardown.
    DomNode* node = ref.address;
    if (reason == DOM_NODE_PIN_WRAPPER) {
        if (!node->wrapper_pin_count) dom_lifecycle_fail("wrapper-underflow", ref, reason);
        --node->wrapper_pin_count;
    }
    dom_observe_pin_change(doc, ref, reason, false);
    dom_node_release_live(node);
    return true;
}

static DomNode* dom_retirement_parent(DomNode* node) {
    DomAttr* attr = node->as_attribute();
    return attr ? static_cast<DomNode*>(attr->owner_element) :
        const_cast<DomNode*>(dom_host_including_parent(node));
}

static bool dom_node_is_within(DomNode* node, DomNode* ancestor) {
    for (; node; node = dom_retirement_parent(node))
        if (node == ancestor) return true;
    return false;
}

void dom_node_schedule_detached(DomDocument* doc, DomNode* root) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !root || root == (DomNode*)doc->root) return;
    if (!root->lifecycle_live || root->doc != doc) return;
    if (!dom_retirement_parent(root) && !dom_own_root(registry, root))
        dom_lifecycle_fail("detached-root", dom_node_ref(root), DOM_NODE_PIN_EXTERNAL);
    for (auto* work = registry->candidates; work; work = work->next)
        if (work->node != root && dom_node_is_within(root, work->node)) return;
    bool scheduled = false;
    for (auto* work = registry->candidates; work;) {
        auto* next = work->next;
        if (root == work->node) scheduled = true;
        else if (dom_node_is_within(work->node, root)) dom_end_candidacy(registry, work);
        work = next;
    }
    if (!scheduled) {
        auto* work = (DomDetachedRoot*)pool_calloc(doc->document_pool, sizeof(DomDetachedRoot));
        if (!work) dom_lifecycle_fail("allocate-detached-root", dom_node_ref(root), DOM_NODE_PIN_EXTERNAL);
        work->node = root;
        root->node_flags |= DOM_NODE_FLAG_DETACHED_CANDIDATE;
        work->next = registry->candidates;
        if (work->next) work->next->prev = work;
        registry->candidates = work;
        ++registry->stats.scheduled_candidates;
    }

    // A detached table can retain anonymous layout boxes beyond the next view
    // pool reset; restore its authored child chain while those boxes are live.
    if (doc->view_tree) view_tree_prepare_detached_subtree(root);
    // Detached wrapper-owned state must stop being a native-tree GC root;
    // a live JS wrapper remains the sole owner until possible reattachment.
    dom_expando_attachment_changed(doc, root, false);
}

void dom_node_cancel_detached(DomDocument* doc, DomNode* root) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !root) return;
    if (!root->lifecycle_live || root->doc != doc) return;
    dom_disown_root(registry, root);
    if (auto* work = dom_find_candidate(registry, root)) {
        dom_end_candidacy(registry, work);
        ++registry->stats.cancelled_candidates;
    }

    // Reinsertion calls this before linking the parent; the lifecycle decision
    // itself confirms that the subtree is becoming attached again.
    dom_expando_attachment_changed(doc, root, true);
}

// the optional byte total charges wrapper-only detached trees for idle GC.
static bool dom_subtree_can_retire(DomDocument* doc, DomNode* node,
                                   bool allow_wrappers = false, size_t* bytes = nullptr) {
    // Layout-only nodes need not be in the script DOM registry. Their view
    // owner releases them; they must not keep an authored subtree alive.
    if (node && node->is_element() && node->as_element()->is_synthetic()) {
        return true;
    }
    if (!node || node == doc->root || !node->lifecycle_live || node->doc != doc || !node->recycling_arena) return false;
    uint32_t allowed = allow_wrappers ? node->wrapper_pin_count : 0;

    if (allow_wrappers && node->external_retain_count > allowed)
        allowed += dom_wrapper_hold_count(doc, node);
    if (node->external_retain_count != allowed) {
        return false;
    }
    if (bytes) *bytes += dom_node_allocation_size(node);
    if (DomElement* element = node->as_element()) {
        DomElement* shadow = element->shadow_root_element();
        if (shadow && !dom_subtree_can_retire(doc, shadow, allow_wrappers, bytes)) return false;
        DomElement* content = element->template_contents();
        if (content && !dom_subtree_can_retire(doc, content, allow_wrappers, bytes)) return false;
        for (DomAttr* attribute = element->rare_ext ? element->rare_ext->attribute_nodes.get() : nullptr;
             attribute; attribute = attribute->attribute_next) {
            if (!dom_subtree_can_retire(doc, attribute, allow_wrappers, bytes)) return false;
        }
        for (DomNode* child = node->as_element()->first_child; child;
             child = child->next_sibling) {
            if (!dom_subtree_can_retire(doc, child, allow_wrappers, bytes)) return false;
        }
    }
    return true;
}

// A rejected candidate held only by script wrappers waits on a collection;
// charge it once per detachment so the host can pace idle-time collections.
static void dom_note_stranded(DomDocument* doc, DomNodeRegistry* registry, DomDetachedRoot* work) {
    if (work->stranded_bytes) return;
    size_t bytes = 0;
    if (!dom_subtree_can_retire(doc, work->node, true, &bytes)) return;
    work->stranded_bytes = bytes;
    registry->stats.wrapper_stranded_bytes += bytes;
}

static size_t dom_retire_subtree(DomDocument* doc, DomNode* node) {

    if (node && node->is_element() && node->as_element()->is_synthetic()) {
        return 0;
    }
    DomNodeRegistry* registry = dom_registry(doc);
    size_t primary_size = dom_node_allocation_size(node);
    Arena* arena = node->recycling_arena;
    // Only a fat node recycles its Lambda result identity. Ordinary DOM
    // wrappers borrow parser values that remain valid after wrapper retirement.
    Item result = ItemNull;
    if (node->is_element() && (node->node_flags & DOM_NODE_FLAG_EMBEDDED_SOURCE)) {
        result.element = &node->as_element()->elmt;
    } else if (node->is_text() && node->as_text()->native_string ==
               (String*)(node->as_text() + 1)) {
        result.item = s2it(node->as_text()->native_string.get());
    }
    if (result.item != ItemNull.item) dom_retire_release_render_result(doc, result);
    dom_slot_release_manual_links(node);
    size_t retired = 0;
    if (node->is_element()) {
        DomElement* element = node->as_element();
        if (DomElement* shadow = element->shadow_root_element()) {
            // the shadow root has an owning host edge even though its DOM parent is null.
            shadow->set_shadow_host_element(nullptr);
            element->set_shadow_root_element(nullptr);
            retired += dom_retire_subtree(doc, shadow);
        }
        if (DomElement* content = element->template_contents()) {
            element->rare_ext->template_contents = nullptr;
            content->rare_ext->template_content_host = nullptr;
            retired += dom_retire_subtree(doc, content);
        }
        while (node->as_element()->rare_ext && node->as_element()->rare_ext->attribute_nodes) {
            DomAttr* attribute = node->as_element()->rare_ext->attribute_nodes;
            dom_attribute_node_detach(attribute);
            retired += dom_retire_subtree(doc, attribute);
        }
        DomNode* child = node->as_element()->first_child;
        while (child) {
            DomNode* next = child->next_sibling;
            dom_unlink_from_sibling_chain(child);
            retired += dom_retire_subtree(doc, child);
            child = next;
        }
        dom_element_release_retired_storage(node->as_element());
    } else if (node->is_text()) {
        dom_text_release_retired_storage(doc, node->as_text());
    } else if (DomAttr* attribute = node->as_attribute()) {
        dom_attribute_node_release_retired_storage(attribute);
    }

    if (node->node_flags & DOM_NODE_FLAG_GC_BACKING)
        dom_managed_backing_release(dom_managed_backing_slot(doc, node));
    dom_end_candidacy(registry, dom_find_candidate(registry, node));
    dom_forget_address(registry, node);
    dom_disown_root(registry, node);
    node->lifecycle_live = false;
    // retired storage holds its own recycle work until the next owner-thread idle slice.
    auto* work = new (node) DomRecycleWork{registry->pending_free, arena, primary_size};
    registry->pending_free = work;
    registry->stats.pending_primary_bytes += primary_size;

    registry->stats.retired_nodes++;
    registry->stats.retired_primary_bytes += primary_size;
    return retired + 1u;
}

static size_t dom_retire_collect(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !registry->candidates || registry->collecting || !doc->node_arena) return 0;
    registry->collecting = true;
    size_t retired = 0;
    registry->next_candidate = registry->candidates;
    while (registry->next_candidate) {
        auto* work = registry->next_candidate;
        registry->next_candidate = work->next;
        DomNode* root = work->node;
        if ((root->is_element() && root->as_element()->is_synthetic()) || dom_retirement_parent(root)) {
            dom_end_candidacy(registry, work);
            ++registry->stats.rejected_attached;
        } else {
            // An embedded document pins its iframe host. Release that
            // ownership edge before testing the detached subtree's pins,
            // otherwise iframe removal forms a retention cycle.
            if (doc->view_tree) {
                view_tree_release_detached_embedded_documents(doc->view_tree, root);
            }
            if (dom_subtree_can_retire(doc, root)) {
                // durable control/media state survives detachment, but must end before its weak node identity retires (D5.5.1).
                view_state_detach_subtree(doc->state, root);
                if (doc->view_tree) {
                    view_tree_release_retired_subtree(doc->view_tree, root);
                } else {
                    // a script can allocate form props before the first view exists.
                    view_pool_release_detached_form_props(root);
                }
                registry->lifetime_stamp = 0;
                retired += dom_retire_subtree(doc, root);
            } else {
                registry->stats.rejected_pinned++;
                dom_note_stranded(doc, registry, work);
            }
        }
    }

    if (retired) registry->lifetime_stamp = dom_next_lifetime_stamp();
    registry->collecting = false;
    return retired;
}

static void dom_retire_recycle_one(DomNodeRegistry* registry) {
    auto* work = registry->pending_free;
    registry->pending_free = work->next;
    Arena* arena = work->arena;
    size_t size = work->size;
    memset(work, 0xdd, size);
    arena_retire(arena, work, size);
    registry->stats.pending_primary_bytes -= size;
    ++registry->stats.recycled_nodes;
}

size_t dom_retire_sweep(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry) return 0;
    if (dom_retirement_deferred && !registry->destroying) {
        registry->sweep_requested = true;
        dom_retire_enqueue(registry);
        return 0;
    }
    size_t retired = dom_retire_collect(doc);
    while (registry->pending_free) dom_retire_recycle_one(registry);
    return retired;
}

bool dom_retire_set_deferred(bool enabled) {
    bool previous = dom_retirement_deferred;
    dom_retirement_deferred = enabled;
    return previous;
}

bool dom_retire_idle(uint64_t budget_us) {
    uint64_t start = time_now_us();
    while (dom_retirement_queue) {
        DomNodeRegistry* registry = dom_retirement_queue;
        dom_retire_dequeue(registry);
        if (registry->sweep_requested) {
            registry->sweep_requested = false;
            dom_retire_collect(registry->document);
        }
        // Arena allocation and recycling stay on their owning thread (D5.1.4).
        // Yield between frees so coalescing a large retired page cannot block
        // the next input event until the entire page has been recycled.
        while (registry->pending_free && time_now_us() - start < budget_us) {
            dom_retire_recycle_one(registry);
        }
        if (registry->pending_free || registry->sweep_requested) dom_retire_enqueue(registry);
        if (time_now_us() - start >= budget_us) break;
    }
    return dom_retirement_queue != nullptr;
}

bool dom_retire_wrapper_collection_due(DomDocument* doc, size_t threshold) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || registry->destroying) return false;
    size_t stranded = registry->stats.wrapper_stranded_bytes;
    return stranded > registry->stranded_after_collection &&
        stranded - registry->stranded_after_collection >= threshold;
}

void dom_retire_wrapper_collection_done(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    // Wrappers that survived the collection are still owned by script; only
    // newly stranded bytes may ask for the next one.
    if (registry) registry->stranded_after_collection = registry->stats.wrapper_stranded_bytes;
}

void dom_retire_begin_destroy(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || registry->destroying) return;
    dom_retire_dequeue(registry);
    registry->destroying = true;
    registry->lifetime_stamp = 0;
    // revoke JS handles before resource callbacks can free an adopted node's physical arena.
    dom_cssom_invalidate_document(doc);
    registry->sweep_requested = false;
    // The owning arenas are about to be destroyed wholesale. Recycling their
    // individual slots here adds coalescing work without saving any memory.
    registry->pending_free = nullptr;
    registry->stats.pending_primary_bytes = 0;
    // Adopted nodes can lose their physical storage when document resources
    // are destroyed. Unlink both ends while every registered node is alive.
    dom_lifecycle_release_slot_links(doc);
}

void dom_lifecycle_get_stats(DomDocument* doc, DomLifecycleStats* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    DomNodeRegistry* registry = dom_registry(doc);
    if (registry) *out = registry->stats;
}

bool dom_js_mutation_records_reserve(DomDocument* doc, int count) {
    if (!doc || count < 0) return false;
    DomJsRuntime* js = &doc->js;
    if (!js->mutation_records) {
        js->mutation_records = js->inline_mutation_records;
        js->mutation_record_capacity = DOM_JS_MUTATION_RECORD_CAP;
    }
    if (count <= js->mutation_record_capacity) return true;
    int capacity = 0;
    if (!lam::grow_capacity(js->mutation_record_capacity, count,
                           DOM_JS_MUTATION_RECORD_CAP, &capacity)) return false;
    // grow outside the arena: turns reuse the journal and teardown releases it.
    DomJsMutationRecord* records = static_cast<DomJsMutationRecord*>(
        mem_alloc(sizeof(DomJsMutationRecord) * capacity, MEM_CAT_LAYOUT));
    if (!records) return false;
    memcpy(records, js->mutation_records,
           sizeof(DomJsMutationRecord) * js->mutation_record_count);
    if (js->mutation_records != js->inline_mutation_records) mem_free(js->mutation_records);
    js->mutation_records = records;
    js->mutation_record_capacity = capacity;
    return true;
}

void dom_js_mutation_records_destroy(DomDocument* doc) {
    if (!doc) return;
    dom_js_mutation_records_reset(doc);
    if (doc->js.mutation_records != doc->js.inline_mutation_records)
        mem_free(doc->js.mutation_records);
    doc->js.mutation_records = doc->js.inline_mutation_records;
    doc->js.mutation_record_capacity = DOM_JS_MUTATION_RECORD_CAP;
}

bool dom_js_mutation_records_truncate(DomDocument* doc, int count) {
    if (!doc || count < 0 || count > doc->js.mutation_record_count) return false;
    for (int i = count; i < doc->js.mutation_record_count; i++) {
        DomJsMutationRecord* record = &doc->js.mutation_records[i];
        if (record->target && record->target_id) {
            dom_node_unpin(doc, {record->target, record->target_id},
                           DOM_NODE_PIN_RECONCILE);
        }
        if (record->parent && record->parent_id) {
            dom_node_unpin(doc, {record->parent, record->parent_id},
                           DOM_NODE_PIN_RECONCILE);
        }
        memset(record, 0, sizeof(*record));
    }
    doc->js.mutation_record_count = count;
    return true;
}

void dom_js_mutation_records_reset(DomDocument* doc) {
    if (!doc) return;
    dom_js_mutation_records_truncate(doc, 0);
    doc->js.mutation_count = 0;
    doc->js.mutation_sequence = 0;
    doc->js.mutation_kind_mask = 0;
    doc->js.mutation_record_overflow = 0;
    doc->js.inline_stylesheet_mutation_count = 0;
    // Mutation records hold raw nodes across the reconcile pass; release all
    // pins before the single quiescent-point sweep.
    dom_retire_sweep(doc);
}
