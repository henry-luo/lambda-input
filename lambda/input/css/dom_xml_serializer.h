#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct StrBuf;
// optional serializer overrides are borrowed for the duration of one element.
typedef struct DomXmlAttributeOverride {
    const char* name;
    const char* value;
    const char* namespace_uri;
    struct DomXmlAttributeOverride* next;
    bool written;
} DomXmlAttributeOverride;
typedef struct DomXmlProjection {
    void* context;
    bool (*include_element)(void* context, void* element);
    DomXmlAttributeOverride* (*attributes)(void* context, void* element);
    bool (*append_children)(void* context, void* element, struct StrBuf* output);
} DomXmlProjection;
bool dom_serialize_xml(void* node, struct StrBuf* output, const DomXmlProjection* projection);


#ifdef __cplusplus
}
struct DomElement;
bool dom_serializes_html_void(DomElement* element);
#endif
