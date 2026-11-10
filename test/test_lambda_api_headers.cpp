#define LAMBDA_API_ONLY 42
#include "lib/strbuf.h"
#if LAMBDA_API_ONLY != 42
#error "library headers must preserve an existing annotation-only selector"
#endif
#undef LAMBDA_API_ONLY

#include "lib/memtrack.h"
#ifdef LAMBDA_API_ONLY
#error "library headers must restore the annotation-only selector"
#endif

#include "lambda/lambda.h"
static_assert(LMD_TYPE_INT > 0, "the full value API must remain available");
static_assert(sizeof(DateTime) == sizeof(uint64_t), "DateTime layout changed");
