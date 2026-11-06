#pragma once

#include <stdbool.h>

struct DomElement;

#ifdef __cplusplus
extern "C" {
#endif
// Image decoding and intrinsic state are provided by the native engine.
bool dom_engine_set_image_source(struct DomElement* element, const char* source, bool* pending);
bool dom_engine_image_natural_size(struct DomElement* element, int* width, int* height);
bool dom_engine_image_rendered_size(struct DomElement* element, int* width, int* height);
#ifdef __cplusplus
}
#endif
