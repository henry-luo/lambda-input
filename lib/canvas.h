#pragma once

#include <stdint.h>

// Native canvas state is copied across the host boundary, never borrowed.
#define RADIANT_CANVAS_FONT_TEXT_MAX 512
struct RadiantCanvasStateSnapshot {
    uint8_t fill_r, fill_g, fill_b, fill_a;
    uint8_t stroke_r, stroke_g, stroke_b, stroke_a;
    uint8_t line_cap, line_join, text_align;
    float line_width, global_alpha;
    char font[RADIANT_CANVAS_FONT_TEXT_MAX];
};
