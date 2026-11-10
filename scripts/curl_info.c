#include <curl/curl.h>
#include <stdio.h>

static void emit(const char* key, const char* value) {
    fputs(key, stdout);
    fputc('=', stdout);
    puts(value ? value : "");
}

int main(void) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->version || info->age < CURLVERSION_ELEVENTH || !info->feature_names || !info->protocols) return 1;
    emit("version", info->version);
    emit("ssl", info->ssl_version);
    emit("ca", info->cainfo);
    for (const char* const* item = info->feature_names; *item; ++item) emit("feature", *item);
    for (const char* const* item = info->protocols; *item; ++item) emit("protocol", *item);
    return 0;
}
