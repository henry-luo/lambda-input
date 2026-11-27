#include "network_resource_manager.h"
#include "font_resource_faces.h"
#include "resource_processor_hook.h"
#include "../input/css/dom_element.hpp"
#include "../input/css/dom_lifecycle.hpp"
#include "lib/mem.h"
#include "lib/time_util.h"

// create a resource whose owned fields are released by resource_release
NetworkResource* network_resource_create(const char* url,
                                         ResourceType type,
                                         ResourcePriority priority,
                                         struct DomElement* owner) {
    NetworkResource* res = (NetworkResource*)mem_calloc(1, sizeof(NetworkResource), MEM_CAT_NETWORK);
    if (!res) return NULL;

    res->url = mem_strdup(url, MEM_CAT_NETWORK);
    if (!res->url) { mem_free(res); return NULL; }
    res->type = type;
    res->priority = priority;
    network_resource_set_owner(res, owner);
    res->state = STATE_PENDING;
    res->dependents = NULL;  // could use arraylist here for dependencies
    res->start_time = time_now_seconds();
    res->ref_count = 1;
    res->http_status_code = 0;

    // Phase 5: timeout and retry defaults
    res->timeout_ms = 30000;  // 30 seconds default
    res->retry_count = 0;
    res->max_retries = 3;

    return res;
}

void network_resource_set_owner(NetworkResource* res, DomElement* owner) {
    res->owner_node = dom_node_ref(owner);
}

DomElement* network_resource_owner(const NetworkResource* res, DomDocument* doc) {
    if (!res || !res->owner_node.address) return nullptr;
    // A queued transfer can outlive its element or see the address reused.
    DomNode* owner = dom_node_ref_validate(doc, res->owner_node);
    return owner ? owner->as_element() : nullptr;
}

namespace {

// free network resource
void free_network_resource(NetworkResource* res) {
    if (!res) return;

    if (res->type == RESOURCE_IMAGE && res->image_surface) {
        const NetworkResourceProcessor* processor = network_resource_processor_get();
        // Decoded images are Radiant objects. IO only requests their release
        // through its lower-owned registration table and never calls Radiant.
        if (processor && processor->release_image) processor->release_image(res);
        res->image_surface = nullptr;
    }

    if (res->type == RESOURCE_FONT && res->user_data) {
        // Font downloads retain every CSS face sharing the deduplicated URL.
        font_resource_face_list_destroy((FontResourceFaceList*)res->user_data);
        res->user_data = NULL;
    }

    mem_free(res->url);
    mem_free(res->referrer_url);
    mem_free(res->local_path);
    mem_free(res->error_message);
    mem_free(res);
}

}  // namespace

// increment reference count
void resource_retain(NetworkResource* res) {
    if (!res) return;
    __sync_fetch_and_add(&res->ref_count, 1);
}

// decrement reference count and free if zero
void resource_release(NetworkResource* res) {
    if (!res) return;
    if (__sync_sub_and_fetch(&res->ref_count, 1) == 0) {
        free_network_resource(res);
    }
}

