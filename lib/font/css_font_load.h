#pragma once

#include <stddef.h>

// Native-only contract used by the DOM bridge and the document font provider.
typedef struct CssFontLoad CssFontLoad;

typedef enum CssFontLoadStatus {
    CSS_FONT_LOAD_PENDING,
    CSS_FONT_LOAD_LOADED,
    CSS_FONT_LOAD_FAILED,
    CSS_FONT_LOAD_SYNTAX,
} CssFontLoadStatus;

typedef struct CssFontLoadFaceInfo {
    const char* family;
    const char* style;
    const char* weight;
} CssFontLoadFaceInfo;
