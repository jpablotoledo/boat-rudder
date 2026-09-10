#include "theme_page.h"
#include "../language_page/language_page.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include "../../utils/theme_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Shown above the list. Mirrors LANGUAGE_PAGE_TITLE in language_page.c.
#define THEME_PAGE_TITLE "Theme"

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

char *theme_page(int epoch, const char *return_url) {
    char *page_tpl = load_template("theme/theme_epoch%d.html", epoch);
    char *item_tpl = load_template("theme/theme-item_epoch%d.html", epoch);
    if (!page_tpl || !item_tpl) {
        free(page_tpl);
        free(item_tpl);
        return NULL;
    }

    char **keys = NULL;
    size_t count = 0;
    list_theme_keys(&keys, &count);

    char safe_return[512];
    language_sanitize_return(return_url, safe_return, sizeof(safe_return));

    char return_enc[1024];
    url_encode(return_enc, safe_return, sizeof(return_enc));

    // Same Mosaic-era reasoning as language_page.c: HTTP/1.0-era clients
    // choke on a relative Location, so epoch 1 skips the cookie-setting
    // /theme/set redirect and links straight to "<return>?theme=xx" - a
    // plain same-origin GET, request_theme_set()'s top query-string
    // precedence picks it up with no redirect involved. The choice only
    // sticks for that one click, not the whole session.
    int direct_link = epoch <= EPOCH_EARLY;
    const char *sep = strchr(safe_return, '?') ? "&amp;" : "?";

    const char *active = request_theme();

    char *items = strdup("");
    for (size_t i = 0; items && i < count; i++) {
        int is_active = strcmp(keys[i], active) == 0;
        const char *active_attr = epoch >= EPOCH_MODERN
            ? (is_active ? " boat-rudder__theme__item--active" : "")
            : (is_active ? " class=\"br-theme__item--active\"" : "");

        char *item;
        if (direct_link) {
            char href[600];
            snprintf(href, sizeof(href), "%s%stheme=%s", safe_return, sep, keys[i]);
            item = render_template(item_tpl, active_attr, href, keys[i]);
        } else {
            item = render_template(item_tpl, active_attr, keys[i], return_enc, keys[i]);
        }
        items = item ? str_append(items, item) : NULL;
        free(item);
    }

    free_theme_keys(keys, count);

    char *result = items ? render_template(page_tpl, THEME_PAGE_TITLE, items) : NULL;

    free(items);
    free(page_tpl);
    free(item_tpl);
    return result;
}
