#include "theme_catalog.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

void list_theme_keys(char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    DIR *d = opendir("./html/themes");
    if (!d) return;

    size_t cap = 8;
    char **keys = malloc(cap * sizeof(char *));
    if (!keys) { closedir(d); return; }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[300];
        snprintf(path, sizeof(path), "./html/themes/%s", entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (count >= cap) {
            cap *= 2;
            char **grown = realloc(keys, cap * sizeof(char *));
            if (!grown) break;
            keys = grown;
        }
        keys[count++] = strdup(entry->d_name);
    }
    closedir(d);

    *out = keys;
    *out_count = count;
}

void free_theme_keys(char **keys, size_t count) {
    if (!keys) return;
    for (size_t i = 0; i < count; i++) free(keys[i]);
    free(keys);
}

int theme_key_is_valid(const char *key) {
    if (!key || !key[0]) return 0;

    // Restrict the charset before ever touching the filesystem: `key`
    // reaches here from a cookie and a public "?key=" query parameter
    // (/theme/set), not just the admin form, so "../etc" (or a CRLF
    // sequence headed for the Set-Cookie header it ends up in) must be
    // rejected outright rather than merely fail the stat() below - a
    // matching real directory elsewhere under html/ would otherwise pass.
    for (const char *p = key; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }

    char path[256];
    int n = snprintf(path, sizeof(path), "./html/themes/%s", key);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
