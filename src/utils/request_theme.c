#include "request_theme.h"
#include "config_loader.h"
#include "theme_catalog.h"
#include "../db/cms_site_settings.h"
#include <stdlib.h>
#include <string.h>

// Per-thread, per-request. See request_theme.h for why this is not a parameter.
static __thread char current_theme[64] = "";

// Reads the value of `name` from a raw Cookie header ("a=1; theme=light; b=2").
// Returns 1 and fills `out` on a match, 0 otherwise. Duplicated from
// request_lang.c's identical private helper - no shared cookie-parsing
// utility exists yet in this codebase.
static int cookie_value(const char *cookie_header, const char *name,
                        char *out, size_t out_size) {
    if (!cookie_header || !name || out_size == 0) return 0;

    size_t name_len = strlen(name);
    const char *p = cookie_header;

    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        const char *eq = strchr(p, '=');
        if (!eq) break;

        if ((size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            const char *val = eq + 1;
            const char *end = strchr(val, ';');
            size_t len = end ? (size_t)(end - val) : strlen(val);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, val, len);
            out[len] = '\0';
            return 1;
        }

        const char *next = strchr(p, ';');
        if (!next) break;
        p = next + 1;
    }
    return 0;
}

static void set_current(const char *key) {
    strncpy(current_theme, key, sizeof(current_theme) - 1);
    current_theme[sizeof(current_theme) - 1] = '\0';
}

void request_theme_set(const char *cookie_header, const char *theme_query) {
    if (theme_query && theme_query[0] && theme_key_is_valid(theme_query)) {
        set_current(theme_query);
        return;
    }

    char cookie_theme[64] = "";
    if (cookie_value(cookie_header, "theme", cookie_theme, sizeof(cookie_theme)) &&
        theme_key_is_valid(cookie_theme)) {
        set_current(cookie_theme);
        return;
    }

    char *key = cms_get_active_theme_key(); // never NULL - see its own doc
    set_current(key);
    free(key);
}

const char *request_theme(void) {
    // Falls back to the raw config value (no DB call) rather than calling
    // request_theme_set()'s own resolution - the setter has simply not run
    // on this thread yet, same edge case request_lang() guards against.
    return current_theme[0] ? current_theme : theme;
}
