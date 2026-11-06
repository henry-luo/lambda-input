#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// Caller owns the normalized comma-separated UI language list (mem_free).
char* dom_platform_preferred_languages(void);
#ifdef __cplusplus
}
#endif
