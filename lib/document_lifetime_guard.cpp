#include "document_lifetime_guard.h"
#include "mem.h"

struct LambdaDocumentLifetimeGuard {
    void* document;
    LambdaDocumentLifetimeGuard* next;
};

static thread_local LambdaDocumentLifetimeGuard* document_lifetime_guards;

bool lambda_document_lifetime_guard_active(void* document) {
    if (!document) return false;
    for (LambdaDocumentLifetimeGuard* guard = document_lifetime_guards; guard; guard = guard->next)
        if (guard->document == document) return true;
    return false;
}

LambdaDocumentLifetimeGuard* lambda_document_lifetime_guard_begin(void* document) {
    if (!document || lambda_document_lifetime_guard_active(document)) return nullptr;
    LambdaDocumentLifetimeGuard* guard = (LambdaDocumentLifetimeGuard*)mem_calloc(
        1, sizeof(LambdaDocumentLifetimeGuard), MEM_CAT_EVAL);
    if (!guard) return nullptr;
    guard->document = document;
    guard->next = document_lifetime_guards;
    document_lifetime_guards = guard;
    return guard;
}

bool lambda_document_lifetime_guard_is_current(LambdaDocumentLifetimeGuard* guard) {
    for (LambdaDocumentLifetimeGuard* entry = document_lifetime_guards; entry; entry = entry->next)
        if (entry == guard) return true;
    return false;
}

bool lambda_document_lifetime_guard_owns(LambdaDocumentLifetimeGuard* guard, void* document) {
    return lambda_document_lifetime_guard_is_current(guard) && guard->document == document;
}

bool lambda_document_lifetime_guard_end(LambdaDocumentLifetimeGuard* guard) {
    for (LambdaDocumentLifetimeGuard** link = &document_lifetime_guards; *link; link = &(*link)->next) {
        if (*link != guard) continue;
        *link = guard->next;
        mem_free(guard);
        return true;
    }
    return false;
}
