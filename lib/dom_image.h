#pragma once

#include <stdbool.h>
#include <stdint.h>

struct DomElement;

#ifdef __cplusplus
extern "C" {
#endif
// Image decoding and intrinsic state are provided by the native engine.
bool dom_engine_set_image_source(struct DomElement* element, const char* source, bool* pending);
bool dom_engine_image_natural_size(struct DomElement* element, int* width, int* height);
bool dom_engine_image_rendered_size(struct DomElement* element, int* width, int* height);
typedef struct DomImageRequestInfo {
    const char* source; // borrowed until this element's next source update
    uint64_t generation;
    bool pending;
    bool available;
} DomImageRequestInfo;
DomImageRequestInfo dom_engine_image_request_info(struct DomElement* element);
bool dom_engine_image_decode_request(struct DomElement* element, uint64_t generation);
#ifdef __cplusplus
}
#endif
