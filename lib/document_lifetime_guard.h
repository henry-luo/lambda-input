#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LambdaDocumentLifetimeGuard LambdaDocumentLifetimeGuard;

// Admission is owner-thread local; a guard protects native document retirement.
LambdaDocumentLifetimeGuard* lambda_document_lifetime_guard_begin(void* document);
bool lambda_document_lifetime_guard_active(void* document);
bool lambda_document_lifetime_guard_is_current(LambdaDocumentLifetimeGuard* guard);
bool lambda_document_lifetime_guard_owns(LambdaDocumentLifetimeGuard* guard, void* document);
bool lambda_document_lifetime_guard_end(LambdaDocumentLifetimeGuard* guard);

#ifdef __cplusplus
}
#endif
