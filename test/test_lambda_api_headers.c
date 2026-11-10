#ifdef LAMBDA_TEST_DATETIME_FIRST
#include "lib/datetime.h"
#endif

#include "lib/memtrack.h"
#include "lib/strbuf.h"

#ifndef LAMBDA_TEST_DATETIME_FIRST
#include "lib/datetime.h"
#endif

#ifdef LAMBDA_API_ONLY
#error "library headers must restore the annotation-only selector"
#endif
#ifndef LAMBDA_LIB_API
#error "library visibility annotations must be available"
#endif

_Static_assert(sizeof(DateTime) == sizeof(uint64_t), "DateTime layout changed");
int lambda_api_headers_c_probe(DateTime* value) { return (int)value->year_month; }
