#pragma once

#include <stdbool.h>

struct DomElement;

// Native layout and the DOM host share copied CSS box measurements.
struct DomCssBoxSizes {
    float content_width, content_height;
    float border_width, border_height;
    float padding_left, padding_top;
    bool vertical;
};

#ifdef __cplusplus
extern "C" {
#endif
bool dom_engine_element_css_boxes(struct DomElement* element, struct DomCssBoxSizes* sizes);
#ifdef __cplusplus
}
#endif
