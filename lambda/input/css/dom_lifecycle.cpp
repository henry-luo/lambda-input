#include "dom_lifecycle.hpp"

#include "dom_element.hpp"
#include "dom_node.hpp"
#include "../../core/data.hpp"
#include "lib/arena.h"
#include "lib/log.h"
#include "lib/time_util.h"
#include "lib/mem_grow.hpp"
#include "lib/memtrack.h"
#include "lib/hashmap_helpers.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint64_t dom_node_ref_identity_hash(DomNodeRef ref, uint64_t seed0, uint64_t seed1) {
    return hashmap_hash_bytes(&ref.expected_id, sizeof(ref.expected_id), seed0, seed1);
}

int dom_node_ref_identity_compare(DomNodeRef left, DomNodeRef right) {
    return left.expected_id < right.expected_id ? -1 : left.expected_id > right.expected_id;
}

struct DomDetachedRoot;

typedef struct DomNodeRecord {
    DomNode* address;
    Arena* primary_arena;
    uint32_t id;
    DomNodeLifeState state;
    bool recyclable;
    bool current_owner;
    bool retiring : 1;
    bool preparing_adoption : 1;
    uint32_t pins[DOM_NODE_PIN_REASON_COUNT];
    DomDetachedRoot* candidate;
    struct DomNodeRecord* bucket_next;
    struct DomNodeRecord* all_next;
    struct DomNodeRecord* all_prev;
    struct DomNodeRecord* retire_next;
} DomNodeRecord;

// only roots awaiting disposal own pending work. Children are reached structurally.
struct DomDetachedRoot {
    DomNodeRecord* record;
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
    DomNodeRecord** buckets;
    size_t bucket_count;
    size_t record_count;
    DomNodeRecord* all_records;
    // Detached candidates only: scheduling a detach compares against these, and
    // scanning every record per replaced node made reactive redraws quadratic.
    DomDetachedRoot* candidates;
    DomDetachedRoot* next_candidate; // cancellations may advance the active traversal
    bool collecting;
    DomDocument* document;
    DomNodeRecord* pending_free;
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

static size_t dom_node_bucket(DomNodeRegistry* registry, DomNode* address) {
    uintptr_t value = (uintptr_t)address;
    value ^= value >> 17u;
    value *= (uintptr_t)0xed5ad4bbu;
    value ^= value >> 11u;
    return (size_t)(value & (registry->bucket_count - 1u));
}

static DomNodeRecord* dom_record_find(DomNodeRegistry* registry, DomNode* address) {
    if (!registry || !registry->buckets || !address) return nullptr;
    size_t bucket = dom_node_bucket(registry, address);
    for (DomNodeRecord* record = registry->buckets[bucket]; record;
         record = record->bucket_next) {
        if (record->address == address) return record;
    }
    return nullptr;
}

static void dom_record_begin_candidacy(DomNodeRegistry* registry, DomNodeRecord* record) {
    auto* work = (DomDetachedRoot*)pool_calloc(registry->document->document_pool, sizeof(DomDetachedRoot));
    if (!work) dom_lifecycle_fail("allocate-detached-root", dom_node_ref(record->address), DOM_NODE_PIN_EXTERNAL);
    work->record = record;
    work->next = registry->candidates;
    if (work->next) work->next->prev = work;
    registry->candidates = work;
    record->candidate = work;
}

static void dom_record_end_candidacy(DomNodeRegistry* registry, DomNodeRecord* record) {
    DomDetachedRoot* work = record->candidate;
    if (!work) return;
    if (registry->next_candidate == work) registry->next_candidate = work->next;
    if (work->prev) work->prev->next = work->next;
    else registry->candidates = work->next;
    if (work->next) work->next->prev = work->prev;
    registry->stats.wrapper_stranded_bytes -= work->stranded_bytes;
    if (registry->stranded_after_collection > registry->stats.wrapper_stranded_bytes)
        registry->stranded_after_collection = registry->stats.wrapper_stranded_bytes;
    record->candidate = nullptr;
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

static bool dom_registry_resize(DomDocument* doc, size_t bucket_count) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !doc->document_pool || bucket_count < 64) return false;
    DomNodeRecord** buckets = (DomNodeRecord**)pool_calloc(
        doc->document_pool, bucket_count * sizeof(DomNodeRecord*));
    if (!buckets) return false;
    DomNodeRecord** old_buckets = registry->buckets;
    size_t old_count = registry->bucket_count;
    registry->buckets = buckets;
    registry->bucket_count = bucket_count;
    for (DomNodeRecord* record = registry->all_records; record;
         record = record->all_next) {
        size_t bucket = dom_node_bucket(registry, record->address);
        record->bucket_next = buckets[bucket];
        buckets[bucket] = record;
    }
    if (old_buckets && old_count) pool_free(doc->document_pool, old_buckets);
    return true;
}

bool dom_lifecycle_init(DomDocument* doc) {
    if (!doc || !doc->document_pool) return false;
    if (doc->services.node_registry) return true;
    DomNodeRegistry* registry = (DomNodeRegistry*)pool_calloc(
        doc->document_pool, sizeof(DomNodeRegistry));
    if (!registry) return false;
    registry->document = doc;
    doc->services.node_registry = registry;
    if (!dom_registry_resize(doc, 256)) {
        doc->services.node_registry = nullptr;
        pool_free(doc->document_pool, registry);
        return false;
    }
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
    if (registry) dom_retire_dequeue(registry);
    // Registry records are document-pool allocations and disappear in the
    // immediately following pool destruction; clearing the owner blocks any
    // teardown callback from validating already-destroyed arena storage.
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

static void dom_publish_record(DomNodeRegistry* registry, DomNodeRecord* record) {
    size_t bucket = dom_node_bucket(registry, record->address);
    record->bucket_next = registry->buckets[bucket];
    registry->buckets[bucket] = record;
    record->all_next = registry->all_records;
    if (record->all_next) record->all_next->all_prev = record;
    registry->all_records = record;
    registry->record_count++;
    registry->stats.registered_nodes++;
}

static DomNodeRecord* dom_reserve_record(DomDocument* doc, size_t unpublished) {
    DomNodeRegistry* registry = dom_registry(doc);
    if ((registry->record_count + unpublished + 1u) * 4u > registry->bucket_count * 3u &&
        !dom_registry_resize(doc, registry->bucket_count * 2u)) return nullptr;
    return (DomNodeRecord*)pool_calloc(doc->document_pool, sizeof(DomNodeRecord));
}

static bool dom_node_registry_register_owned(DomDocument* doc, DomNode* node,
        Arena* primary_arena, uint32_t id,
        size_t primary_size, bool recyclable) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !node || !id || !primary_size || primary_size != dom_node_allocation_size(node)) return false;
    if (recyclable && !primary_arena) {
        log_error("DOM_LIFECYCLE_INVARIANT: recyclable node %p has no owning arena",
                  (void*)node);
        return false;
    }
    DomNodeRecord* record = dom_record_find(registry, node);
    if (record) {
        if (record->state != DOM_NODE_RETIRED) {
            if (record->current_owner && record->id == id) return true;
            dom_lifecycle_fail("duplicate-live-register", dom_node_ref(node),
                               DOM_NODE_PIN_EXTERNAL);
            return false;
        }
        memset(record->pins, 0, sizeof(record->pins));
        record->id = id;
        record->primary_arena = primary_arena;
        record->state = DOM_NODE_LIVE;
        record->recyclable = recyclable;
        dom_record_end_candidacy(registry, record);
        record->current_owner = true;
        registry->stats.reused_addresses++;
        registry->stats.registered_nodes++;
        return true;
    }

    record = dom_reserve_record(doc, 0);
    if (!record) return false;
    record->address = node;
    record->primary_arena = primary_arena;
    record->id = id;
    record->state = DOM_NODE_LIVE;
    record->recyclable = recyclable;
    record->current_owner = true;
    dom_publish_record(registry, record);
    return true;
}

bool dom_node_registry_register(DomDocument* doc, DomNode* node,
                                size_t primary_size, bool recyclable) {
    Arena* primary_arena = dom_node_primary_arena(doc, node);
    if (!node || !dom_node_registry_register_owned(doc, node, primary_arena, node->id,
                                                   primary_size, recyclable)) return false;
    node->doc = lam::up(doc);
    return true;
}

static void dom_record_unlink_free(DomNodeRegistry* registry, DomNodeRecord* record);

static bool dom_record_has_pins(const DomNodeRecord* record) {
    for (uint32_t count : record->pins) {
        if (count) return true;
    }
    return false;
}

struct DomAdoptionStep {
    DomNodeRecord* old_record;
    DomNodeRecord* new_record;
    uint32_t id;
    bool unpublished;
};

static bool dom_prepare_adoption_step(DomDocument* source, DomDocument* destination,
        DomNode* node, uint32_t id, size_t unpublished, DomAdoptionStep* step) {
    if (!source || !dom_registry(destination) || source == destination || !node || !id) return false;
    auto* old = dom_record_find(dom_registry(source), node);
    if (!old || !old->current_owner || old->preparing_adoption ||
        old->state == DOM_NODE_RETIRED || old->id != node->id)
        return false;
    auto* next = dom_record_find(dom_registry(destination), node);
    if (next && (next->current_owner || next->state == DOM_NODE_RETIRED)) return false;
    if (node->is_element() && !node->as_element()->preserve_storage_owner()) return false;
    if (node->external_retain_count && !dom_wrapper_adopt_document(source, destination, node, false)) return false;
    if (!dom_slot_prepare_adoption(source, destination, node)) return false;
    bool fresh = !next;
    if (fresh) next = dom_reserve_record(destination, unpublished);
    if (!next) {
        dom_slot_finish_adoption(source, destination, node, false);
        return false;
    }
    // an old-document lease keeps its original generation when the node returns.
    old->preparing_adoption = true;
    *step = {old, next, !fresh && dom_record_has_pins(next) ? next->id : id, fresh};
    return true;
}

static void dom_finish_adoption_step(DomDocument* source, DomDocument* destination,
        DomAdoptionStep step, bool commit) {
    auto* source_record = step.old_record;
    auto* destination_record = step.new_record;
    DomNode* node = source_record->address;
    source_record->preparing_adoption = false;
    if (!commit) {
        dom_slot_finish_adoption(source, destination, node, false);
        if (step.unpublished) pool_free(destination->document_pool, destination_record);
        return;
    }
    dom_node_refresh_managed_backing(source, node);
    destination_record->address = node;
    destination_record->id = step.id;
    destination_record->primary_arena = source_record->primary_arena;
    destination_record->state = DOM_NODE_LIVE;
    destination_record->recyclable = source_record->recyclable;
    destination_record->current_owner = true;
    if (step.unpublished) dom_publish_record(dom_registry(destination), destination_record);
    if (node->node_flags & DOM_NODE_FLAG_GC_BACKING) {
        // keep the slot address and the registering heap stable across adoption.
        DomManagedBacking** slot = dom_managed_backing_slot(source, node);
        DomManagedBacking* root = *slot;
        *slot = root->next;
        root->next = dom_registry(destination)->managed_roots;
        dom_registry(destination)->managed_roots = root;
    }
    // The source registry may still own wrapper/expando pins that must unpin
    // normally, but its detached candidate must never recycle adopted storage.
    dom_record_end_candidacy(dom_registry(source), source_record);
    source_record->state = DOM_NODE_LIVE;
    source_record->current_owner = false;
    node->id = step.id;
    node->doc = lam::up(destination);
    if (node->external_retain_count) dom_wrapper_adopt_document(source, destination, node, true);
    if (node->is_element() && node->as_element()->form_storage()) form_control_adopt_document(node->as_element());
    dom_slot_finish_adoption(source, destination, node, true);
    if (!dom_record_has_pins(source_record)) dom_record_unlink_free(dom_registry(source), source_record);
}

bool dom_node_registry_transfer(DomDocument* source, DomDocument* destination,
        DomNode* node, uint32_t* destination_id) {
    DomAdoptionStep step;
    if (!destination_id ||
        !dom_prepare_adoption_step(source, destination, node, *destination_id, 0, &step)) return false;
    dom_finish_adoption_step(source, destination, step, true);
    *destination_id = step.id;
    return true;
}

struct DomAdoptionBatch {
    DomDocument* source;
    DomDocument* destination;
    DomAdoptionStep* steps;
    size_t count, capacity, unpublished;
};

static bool dom_prepare_adoption_subtree(DomAdoptionBatch* batch, DomNode* node) {
    if (!lam::pool_grow_array(batch->destination->document_pool, &batch->steps,
            &batch->capacity, batch->count + 1, 16)) return false;
    DomAdoptionStep& step = batch->steps[batch->count];
    if (!dom_prepare_adoption_step(batch->source, batch->destination, node,
        dom_document_alloc_node_id(batch->destination), batch->unpublished, &step)) return false;
    ++batch->count;
    batch->unpublished += step.unpublished;
    DomElement* element = node->as_element();
    if (!element) return true;
    for (DomAttr* attr = element->rare_ext ? element->rare_ext->attribute_nodes.get() : nullptr;
         attr; attr = attr->attribute_next)
        if (!dom_prepare_adoption_subtree(batch, attr)) return false;
    for (DomNode* child = element->first_child; child; child = child->next_sibling)
        if (!dom_prepare_adoption_subtree(batch, child)) return false;
    DomElement* shadow = element->shadow_root_element();
    return !shadow || dom_prepare_adoption_subtree(batch, shadow);
}

bool dom_adopt_detached_subtree(DomDocument* source, DomDocument* destination, DomNode* node) {
    if (!source || !destination || !node || node->parent || !dom_registry(destination)) return false;
    DomAdoptionBatch batch = {source, destination};
    bool prepared = dom_prepare_adoption_subtree(&batch, node);
    for (size_t i = 0; i < batch.count; ++i) {
        DomNode* member = batch.steps[i].old_record->address;
        dom_finish_adoption_step(source, destination, batch.steps[i], prepared);
        if (prepared) member->view_state_ref = nullptr;
    }
    if (prepared) {
        // owner callbacks can inspect other members; publish the complete closure first.
        for (size_t i = 0; i < batch.count; ++i) {
            DomNode* member = batch.steps[i].new_record->address;
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
    DomNodeRef ref = {node, node ? node->id : 0};
    return ref;
}

DomNode* dom_node_ref_validate(DomDocument* doc, DomNodeRef ref) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !ref.address || !ref.expected_id) return nullptr;
    // Lookup is address-only and intentionally does not read the target bytes;
    // an arena slot may already contain a different node generation.
    DomNodeRecord* record = dom_record_find(registry, ref.address);
    if (!record || record->state == DOM_NODE_RETIRED || record->id != ref.expected_id) {
        registry->stats.stale_ref_rejections++;
        return nullptr;
    }
    return ref.address;
}

bool dom_node_registry_owns(DomDocument* doc, DomNode* node) {
    DomNodeRecord* record = dom_record_find(dom_registry(doc), node);
    return record && record->current_owner && record->state != DOM_NODE_RETIRED &&
        record->id == node->id;
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

bool dom_node_pin(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || reason >= DOM_NODE_PIN_REASON_COUNT) return false;
    DomNodeRecord* record = dom_record_find(registry, ref.address);
    if (!record || record->state == DOM_NODE_RETIRED || record->id != ref.expected_id) {
        dom_lifecycle_fail("pin-stale", ref, reason);
        return false;
    }
    dom_node_retain_live(ref.address);
    record->pins[reason]++;
    return true;
}

bool dom_node_unpin(DomDocument* doc, DomNodeRef ref, DomNodePinReason reason) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || reason >= DOM_NODE_PIN_REASON_COUNT) return false;
    DomNodeRecord* record = dom_record_find(registry, ref.address);
    if (!record || record->state == DOM_NODE_RETIRED || record->id != ref.expected_id ||
        record->pins[reason] == 0) {
        dom_lifecycle_fail("unpin-stale-or-underflow", ref, reason);
        return false;
    }
    dom_node_release_live(ref.address);
    record->pins[reason]--;
    if (!record->current_owner && !dom_record_has_pins(record)) dom_record_unlink_free(registry, record);
    return true;
}

uint32_t dom_node_pin_count(DomDocument* doc, DomNodeRef ref,
                            DomNodePinReason reason) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || reason >= DOM_NODE_PIN_REASON_COUNT) return 0;
    DomNodeRecord* record = dom_record_find(registry, ref.address);
    return record && record->id == ref.expected_id ? record->pins[reason] : 0;
}

static DomNode* dom_retirement_parent(DomNode* node) {
    DomAttr* attr = node->as_attribute();
    return attr ? static_cast<DomNode*>(attr->owner_element) : dom_shadow_including_parent(node);
}

static bool dom_node_is_within(DomNode* node, DomNode* ancestor) {
    for (; node; node = dom_retirement_parent(node))
        if (node == ancestor) return true;
    return false;
}

void dom_node_schedule_detached(DomDocument* doc, DomNode* root) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !root || root == (DomNode*)doc->root) return;
    DomNodeRecord* record = dom_record_find(registry, root);
    if (!record || !record->current_owner || record->id != root->id ||
        record->state == DOM_NODE_RETIRED) return;
    // a root inside an already detached candidate is covered by that candidate;
    // otherwise this root subsumes the candidates nested inside it.
    for (auto* work = registry->candidates; work; work = work->next) {
        DomNodeRecord* other = work->record;
        if (other != record && dom_node_is_within(root, other->address)) return;
    }
    for (auto* work = registry->candidates; work;) {
        DomNodeRecord* other = work->record;
        DomDetachedRoot* next = work->next;
        if (other != record && dom_node_is_within(other->address, root)) {
            dom_record_end_candidacy(registry, other);
            other->state = DOM_NODE_LIVE;
        }
        work = next;
    }
    if (!record->candidate) {
        dom_record_begin_candidacy(registry, record);
        record->state = DOM_NODE_DETACHED_CANDIDATE;
        registry->stats.scheduled_candidates++;
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
    DomNodeRecord* record = dom_record_find(registry, root);
    if (!record || !record->current_owner || record->id != root->id ||
        record->state == DOM_NODE_RETIRED) return;
    if (record && record->candidate && record->id == root->id) {
        dom_record_end_candidacy(registry, record);
        record->state = DOM_NODE_LIVE;
        registry->stats.cancelled_candidates++;
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
    DomNodeRegistry* registry = dom_registry(doc);
    DomNodeRecord* record = dom_record_find(registry, node);
    // A retired descendant can remain in a detached parent's old sibling
    // chain while the registry is the only safe source of its generation.
    if (!record || record->state == DOM_NODE_RETIRED || !record->recyclable ||
        record->id != node->id) {
        return false;
    }
    // holds belong to the physical node, including holds issued before adoption.
    uint32_t allowed = allow_wrappers ? record->pins[DOM_NODE_PIN_WRAPPER] : 0;
    if (allow_wrappers && node->external_retain_count > allowed)
        allowed += dom_wrapper_hold_count(doc, node);
    if (node->external_retain_count != allowed) {
        return false;
    }
    if (bytes) *bytes += dom_node_allocation_size(node);
    if (node->is_element()) {
        DomElement* shadow = node->as_element()->shadow_root_element();
        if (shadow && !dom_subtree_can_retire(doc, shadow, allow_wrappers, bytes)) return false;
        for (DomAttr* attribute = node->as_element()->rare_ext ? node->as_element()->rare_ext->attribute_nodes.get() : nullptr;
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
static void dom_record_note_stranded(DomDocument* doc, DomNodeRegistry* registry,
                                     DomNodeRecord* record) {
    if (record->candidate->stranded_bytes) return;
    size_t bytes = 0;
    if (!dom_subtree_can_retire(doc, record->address, true, &bytes)) return;
    record->candidate->stranded_bytes = bytes;
    registry->stats.wrapper_stranded_bytes += bytes;
}

static DomNode* dom_retire_resolve_edge(DomNodeRegistry* registry, DomNode* node,
                                       bool forward) {
    DomNode* live = node;
    while (live) {
        DomNodeRecord* record = dom_record_find(registry, live);
        if (!record || !record->retiring) break;
        live = forward ? live->next_sibling : live->prev_sibling;
    }
    // Compress shared stale chains before any node storage is reclaimed.
    while (node != live) {
        // the two edges have different kinds (next owns, prev is a back link)
        DomNode* next = forward ? node->next_sibling : node->prev_sibling;
        if (forward) node->next_sibling = lam::own(live);
        else node->prev_sibling = lam::up(live);
        node = next;
    }
    return live;
}

static void dom_retire_unlink_inbound_edges(DomNodeRegistry* registry) {
    for (DomNodeRecord* record = registry->all_records; record;
         record = record->all_next) {
        registry->stats.retirement_edge_visits++;
        if (!record->current_owner || record->state == DOM_NODE_RETIRED) continue;
        DomNode* other = record->address;
        other->next_sibling = lam::own(dom_retire_resolve_edge(registry, other->next_sibling, true));
        other->prev_sibling = lam::up(dom_retire_resolve_edge(registry, other->prev_sibling, false));
        if (other->is_element()) {
            DomElement* element = other->as_element();
            element->first_child = lam::own(dom_retire_resolve_edge(registry, element->first_child, true));
            element->last_child = lam::up(dom_retire_resolve_edge(registry, element->last_child, false));
        }
    }
}

static size_t dom_retire_subtree(DomDocument* doc, DomNode* node,
                                 DomNodeRecord** pending) {
    if (node && node->is_element() && node->as_element()->is_synthetic()) {
        return 0;
    }
    DomNodeRegistry* registry = dom_registry(doc);
    DomNodeRecord* record = dom_record_find(registry, node);
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
            retired += dom_retire_subtree(doc, shadow, pending);
        }
        while (node->as_element()->rare_ext && node->as_element()->rare_ext->attribute_nodes) {
            DomAttr* attribute = node->as_element()->rare_ext->attribute_nodes;
            dom_attribute_node_detach(attribute);
            retired += dom_retire_subtree(doc, attribute, pending);
        }
        DomNode* child = node->as_element()->first_child;
        while (child) {
            DomNode* next = child->next_sibling;
            retired += dom_retire_subtree(doc, child, pending);
            child = next;
        }
        dom_element_release_retired_storage(node->as_element());
    } else if (node->is_text()) {
        dom_text_release_retired_storage(doc, node->as_text());
    } else if (DomAttr* attribute = node->as_attribute()) {
        dom_attribute_node_release_retired_storage(attribute);
    }

    size_t primary_size = dom_node_allocation_size(node);
    if (node->node_flags & DOM_NODE_FLAG_GC_BACKING)
        dom_managed_backing_release(dom_managed_backing_slot(doc, node));
    dom_record_end_candidacy(registry, record);
    record->state = DOM_NODE_RETIRED;
    record->retiring = true;
    record->retire_next = *pending;
    *pending = record;
    registry->stats.retired_nodes++;
    registry->stats.retired_primary_bytes += primary_size;
    return retired + 1u;
}

static size_t dom_retire_collect(DomDocument* doc) {
    DomNodeRegistry* registry = dom_registry(doc);
    if (!registry || !registry->candidates || registry->collecting || !doc->node_arena) return 0;
    registry->collecting = true;
    size_t retired = 0;
    DomNodeRecord* pending = nullptr;
    registry->next_candidate = registry->candidates;
    while (registry->next_candidate) {
        DomNodeRecord* record = registry->next_candidate->record;
        registry->next_candidate = registry->next_candidate->next;
        if (record->candidate && record->state == DOM_NODE_DETACHED_CANDIDATE) {
            DomNode* root = record->address;
            if (!dom_node_ref_validate(doc, {root, record->id})) {
                dom_record_end_candidacy(registry, record);
            } else if (root->is_element() && root->as_element()->is_synthetic()) {
                // synthetic layout nodes are released by their view owner;
                // lifecycle retirement must not recycle their DOM addresses.
                dom_record_end_candidacy(registry, record);
                record->state = DOM_NODE_LIVE;
            } else if (dom_retirement_parent(root)) {
                dom_record_end_candidacy(registry, record);
                record->state = DOM_NODE_LIVE;
                registry->stats.rejected_attached++;
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
                    retired += dom_retire_subtree(doc, root, &pending);
                } else {
                    registry->stats.rejected_pinned++;
                    dom_record_note_stranded(doc, registry, record);
                }
            }
        }
    }
    if (pending) {
        // MarkEditor can leave raw inbound edges outside a detached subtree.
        // Repair all of them once per batch, while sibling chains remain live.
        dom_retire_unlink_inbound_edges(registry);
        while (pending) {
            DomNodeRecord* record = pending;
            pending = record->retire_next;
            record->retire_next = nullptr;
            record->retiring = false;
            if (!registry->destroying) {
                record->retire_next = registry->pending_free;
                registry->pending_free = record;
                registry->stats.pending_primary_bytes += dom_node_allocation_size(record->address);
            }
        }
    }
    registry->collecting = false;
    return retired;
}

// Every lookup treats a missing record exactly like a retired one, so a
// recycled slot's record only cost memory and lengthened each detach scan.
static void dom_record_unlink_free(DomNodeRegistry* registry, DomNodeRecord* record) {
    if (record->candidate) dom_record_end_candidacy(registry, record);
    DomNodeRecord** edge = &registry->buckets[dom_node_bucket(registry, record->address)];
    while (*edge && *edge != record) edge = &(*edge)->bucket_next;
    if (*edge) *edge = record->bucket_next;
    if (record->all_prev) record->all_prev->all_next = record->all_next;
    else registry->all_records = record->all_next;
    if (record->all_next) record->all_next->all_prev = record->all_prev;
    registry->record_count--;
    pool_free(registry->document->document_pool, record);
}

static void dom_retire_recycle_one(DomNodeRegistry* registry) {
    DomNodeRecord* record = registry->pending_free;
    registry->pending_free = record->retire_next;
    record->retire_next = nullptr;
    // Edges and generation refs are invalidated before a slot becomes reusable.
    size_t size = dom_node_allocation_size(record->address);
    memset(record->address, 0xdd, size);
    // DOM nodes are the sanctioned arena reuse case: the slot is retained on
    // its arena's retired list for later node allocations, never discarded.
    arena_retire(record->primary_arena, record->address, size);
    registry->stats.pending_primary_bytes -= size;
    registry->stats.recycled_nodes++;
    dom_record_unlink_free(registry, record);
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
