#ifndef NETWORK_PRIORITY_H
#define NETWORK_PRIORITY_H

// lower values are scheduled first; keep policy users independent of worker atomics.
typedef enum {
    PRIORITY_CRITICAL = 0,  // main HTML document
    PRIORITY_HIGH     = 1,  // CSS, fonts
    PRIORITY_NORMAL   = 2,  // images, SVG
    PRIORITY_LOW      = 3   // prefetch, async scripts
} ResourcePriority;

#endif
