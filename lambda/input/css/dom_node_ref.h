#ifndef DOM_NODE_REF_H
#define DOM_NODE_REF_H

#include <stdint.h>

struct DomNode;

typedef struct DomNodeRef {
    struct DomNode* address;
    uint32_t expected_id;
    uint32_t lifetime_stamp; // zero for an address-only probe; minted references are copied intact
} DomNodeRef;

#endif
