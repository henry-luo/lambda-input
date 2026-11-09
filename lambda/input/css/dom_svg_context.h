#pragma once
#include "lib/rdt_geometry_types.h"
#include <stdbool.h>

struct DomDocument;
struct DomElement;
struct RdtLogicalPoint;

#ifdef __cplusplus
extern "C" {
#endif
bool dom_svg_conditions_match(const char* extensions, const char* languages, const char* preferences);
const char* dom_document_preferred_languages(void* document);
bool dom_svg_element_is_eligible(void* element);
void* dom_svg_switch_selected_child(void* element);
#ifdef __cplusplus
}
#endif

typedef bool (*SvgPathContainsFn)(const RdtPath*, const RdtMatrix*, RdtFillRule, const RdtLogicalPoint*);

// synchronous source scopes borrow their previous document/instance/registry; they transfer no ownership.
struct DomSvgSourceContext {
    DomDocument* document;
    DomElement* instance;
    void* registry;
};
