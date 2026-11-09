#include "dom_blob_url.hpp"
#include "dom_element.hpp"
#include "lib/mem.h"
#include "lib/strbuf.h"
#include "lib/url.h"
#include <stdint.h>
#include <string.h>

struct DomBlobUrl : DomDocumentResourceData {
    char* url;
    char* data_uri;
};

static uint64_t blob_url_next_id = 1;

static void blob_url_destroy(DomDocumentResourceData* data) {
    DomBlobUrl* entry = (DomBlobUrl*)data;
    mem_free(entry->url);
    mem_free(entry->data_uri);
    mem_free(entry);
}

static DomBlobUrl* blob_url_find(DomDocument* document, const char* url) {
    if (!document || !url) return nullptr;
    for (DomDocumentResource* resource = document->resources; resource; resource = resource->next) {
        if (resource->destroy != blob_url_destroy) continue;
        DomBlobUrl* entry = (DomBlobUrl*)resource->data.get();
        if (strcmp(entry->url, url) == 0) return entry;
    }
    return nullptr;
}

const char* dom_blob_url_create(DomDocument* document, char* data_uri) {
    if (!document || !data_uri) { mem_free(data_uri); return nullptr; }
    DomBlobUrl* entry = (DomBlobUrl*)mem_calloc(1, sizeof(DomBlobUrl), MEM_CAT_SYSTEM);
    if (!entry) { mem_free(data_uri); return nullptr; }
    entry->data_uri = data_uri;
    StrBuf* url = strbuf_new();
    if (!url) { blob_url_destroy(entry); return nullptr; }
    const char* origin = document->url ? url_get_origin(document->url) : nullptr;
    uint64_t id = __atomic_fetch_add(&blob_url_next_id, 1, __ATOMIC_RELAXED);
    strbuf_append_format(url, "blob:%s/%llu", origin && *origin ? origin : "null", (unsigned long long)id);
    entry->url = mem_dup_n(url->str, url->length, MEM_CAT_SYSTEM);
    strbuf_free(url);
    // Resource bytes belong to the document and can outlive the creating runtime.
    if (!entry->url || !dom_document_add_resource(document, entry, blob_url_destroy)) {
        blob_url_destroy(entry);
        return nullptr;
    }
    return entry->url;
}

const char* dom_blob_url_data_uri(DomDocument* document, const char* url) {
    DomBlobUrl* entry = blob_url_find(document, url);
    return entry ? entry->data_uri : nullptr;
}

void dom_blob_url_revoke(DomDocument* document, const char* url) {
    DomBlobUrl* entry = blob_url_find(document, url);
    if (entry) dom_document_release_resource(document, entry);
}
