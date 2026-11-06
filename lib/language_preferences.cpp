#include "language_preferences.h"
#include "strbuf.h"
#include "str.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef __APPLE__
#define Rect MacOSRect
#include <CoreFoundation/CoreFoundation.h>
#undef Rect
#elif defined(_WIN32)
#include <windows.h>
#endif

static void dom_language_append(StrBuf* output, const char* language, size_t length) {
    while (length && str_char_is_ascii_space(*language)) { language++; length--; }
    while (length && str_char_is_ascii_space(language[length - 1])) length--;
    size_t end = 0;
    while (end < length && language[end] != '.' && language[end] != '@') end++;
    if (!end || str_icmp(language, end, "C", 1) == 0 || str_icmp(language, end, "POSIX", 5) == 0) return;
    if (output->length) strbuf_append_char(output, ',');
    for (size_t index = 0; index < end; index++) strbuf_append_char(output, language[index] == '_' ? '-' : language[index]);
}

extern "C" char* dom_platform_preferred_languages(void) {
    StrBuf* output = strbuf_new(); if (!output) return nullptr;
    // LANGUAGE supplies an explicit preference list; otherwise use the platform UI settings.
    const char* environment = getenv("LANGUAGE");
    if (environment && *environment) {
        const char* start = environment;
        for (const char* cursor = environment;; cursor++) if (!*cursor || *cursor == ':' || *cursor == ',') {
            dom_language_append(output, start, (size_t)(cursor - start));
            if (!*cursor) break;
            start = cursor + 1;
        }
    } else {
#ifdef __APPLE__
        CFArrayRef languages = CFLocaleCopyPreferredLanguages();
        for (CFIndex index = 0, count = languages ? CFArrayGetCount(languages) : 0; index < count; index++) {
            CFStringRef language = (CFStringRef)CFArrayGetValueAtIndex(languages, index);
            char text[256];
            if (language && CFStringGetCString(language, text, sizeof(text), kCFStringEncodingUTF8))
                dom_language_append(output, text, strlen(text));
        }
        if (languages) CFRelease(languages);
#elif defined(_WIN32)
        ULONG count = 0, length = 0;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length) && length) {
            WCHAR* languages = (WCHAR*)mem_calloc(length, sizeof(WCHAR), MEM_CAT_LAYOUT);
            if (languages && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages, &length)) {
                for (const WCHAR* language = languages; *language; language += wcslen(language) + 1) {
                    char text[256];
                    if (WideCharToMultiByte(CP_UTF8, 0, language, -1, text, sizeof(text), nullptr, nullptr))
                        dom_language_append(output, text, strlen(text));
                }
            }
            mem_free(languages);
        }
#else
        const char* language = getenv("LC_ALL");
        if (!language || !*language) language = getenv("LC_MESSAGES");
        if (!language || !*language) language = getenv("LANG");
        if (language) dom_language_append(output, language, strlen(language));
#endif
    }
    char* result = mem_strdup(output->str, MEM_CAT_LAYOUT); strbuf_free(output);
    return result;
}

