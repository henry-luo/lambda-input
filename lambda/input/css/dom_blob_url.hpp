#pragma once

struct DomDocument;

#ifdef __cplusplus
extern "C" {
#endif

// Creation consumes data_uri even on failure; returned bytes belong to the document.
const char* dom_blob_url_create(DomDocument* document, char* data_uri);
const char* dom_blob_url_data_uri(DomDocument* document, const char* url);
void dom_blob_url_revoke(DomDocument* document, const char* url);

#ifdef __cplusplus
}
#endif
