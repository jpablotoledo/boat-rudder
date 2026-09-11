#include "template_utils.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *str_replace_first(const char *src, const char *needle, const char *replacement) {
    const char *pos = strstr(src, needle);
    if (!pos) return strdup(src);

    size_t prefix_len      = (size_t)(pos - src);
    size_t needle_len      = strlen(needle);
    size_t replacement_len = strlen(replacement);
    size_t suffix_len      = strlen(pos + needle_len);

    char *result = malloc(prefix_len + replacement_len + suffix_len + 1);
    if (!result) return NULL;

    memcpy(result, src, prefix_len);
    memcpy(result + prefix_len, replacement, replacement_len);
    memcpy(result + prefix_len + replacement_len, pos + needle_len, suffix_len + 1);

    return result;
}

char *render_template(const char *tpl, ...) {
    va_list args1, args2;
    va_start(args1, tpl);
    va_copy(args2, args1);

    int len = vsnprintf(NULL, 0, tpl, args1);
    va_end(args1);
    if (len < 0) {
        va_end(args2);
        return NULL;
    }

    char *result = malloc((size_t)len + 1);
    if (!result) {
        va_end(args2);
        return NULL;
    }

    vsnprintf(result, (size_t)len + 1, tpl, args2);
    va_end(args2);

    return result;
}

char *str_append(char *dst, const char *src) {
    size_t dst_len = dst ? strlen(dst) : 0;
    size_t src_len = strlen(src);

    char *result = realloc(dst, dst_len + src_len + 1);
    if (!result) {
        free(dst);
        return NULL;
    }

    memcpy(result + dst_len, src, src_len + 1);
    return result;
}

char *build_title_tag(const char *page_title) {
    return render_template("<title>%s</title>", page_title);
}

char *slugify(const char *name) {
    if (!name) return strdup("");
    size_t len = strlen(name);
    char *slug = malloc(len + 1);
    if (!slug) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') {
            slug[j++] = (char)(c + 32);
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            slug[j++] = (char)c;
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j > 0 && slug[j - 1] != '-') slug[j++] = '-';
        }
    }
    while (j > 0 && slug[j - 1] == '-') j--;
    slug[j] = '\0';
    return slug;
}

char *capitalize_first(const char *s) {
    if (!s) return NULL;
    char *out = strdup(s);
    if (out && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
    return out;
}

char *image_url_variant(const char *url, const char *suffix) {
    if (!url || !url[0]) return strdup("");
    if (!suffix || !suffix[0]) return strdup(url);

    const char *dot = strrchr(url, '.');
    if (!dot || dot == url) return strdup(url);

    size_t prefix_len = (size_t)(dot - url);
    size_t suffix_len = strlen(suffix);
    size_t ext_len    = strlen(dot);
    size_t total      = prefix_len + suffix_len + ext_len + 1;

    char *result = malloc(total);
    if (!result) return NULL;
    memcpy(result, url, prefix_len);
    memcpy(result + prefix_len, suffix, suffix_len);
    memcpy(result + prefix_len + suffix_len, dot, ext_len + 1);
    return result;
}
