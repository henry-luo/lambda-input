#include "base64.h"

#ifdef LAMBDA_API_ONLY
#error "library headers must restore the annotation-only selector"
#endif
#ifndef LAMBDA_LIB_API
#error "library visibility annotations must be available"
#endif
