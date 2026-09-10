#include "menu.h"
#include "../../db/cms_languages.h"
#include "../../db/language_catalog.h"
#include "../../db/cms_menu.h"
#include "../../db/cms_site_settings.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/request_lang.h"
#include "../../utils/request_theme.h"
#include "../../utils/request_user.h"
#include "../../utils/template_utils.h"
#include "../../utils/theme_catalog.h"
#include <stdlib.h>
#include <string.h>

// Used when the `menu` collection is empty, unreachable, or mongodb is not
// ready, so the nav bar is never empty.
static const CmsMenuItem FALLBACK_ITEMS[] = {
    {.link = "/", .name = "Home"},
};

#define FALLBACK_ITEM_COUNT (sizeof(FALLBACK_ITEMS) / sizeof(FALLBACK_ITEMS[0]))

// Builds the language selector appended to the nav bar. Epoch 3 renders a
// drop-down whose entries link straight to /language/set; older epochs, where
// a drop-down cannot be relied on, link to the /language page instead. Both
// paths end at the same place - a plain GET that sets the cookie and bounces
// back - so switching language never needs JavaScript.
//
// Returns a malloc'd string ("" when there is nothing to offer: fewer than two
// languages, or the templates are missing), never NULL unless allocation fails.
static char *language_selector(int epoch) {
    CmsLanguageItem *langs = NULL;
    size_t count = 0;
    if (mongodb_manager_is_ready()) cms_get_languages(&langs, &count);

    // A single language offers no choice; showing the control would be noise.
    if (count < 2) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    // The button is tight on space, so it shows the short form ("Esp"); the
    // list has room for the language's own name ("Espanol"), which is what a
    // reader looking for their language actually scans for.
    const char *active = request_lang();
    const char *active_abbr = language_catalog_abbr(active);

    // Come back to the page actually being read. `current_url` is the menu's
    // section (always "/blog" for an article), which would drop the reader on
    // the listing instead.
    char return_enc[1024];
    url_encode(return_enc, request_path(), sizeof(return_enc));

    char *tpl_path = generate_url_theme("menu/menu-lang_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        cms_languages_free(langs, count);
        return strdup("");
    }

    char *result = NULL;
    if (epoch >= EPOCH_MODERN) {
        char *item_path = generate_url_theme("menu/menu-lang-item_epoch%d.html", epoch);
        char *item_tpl = item_path ? read_file_to_string(item_path) : NULL;
        free(item_path);

        if (item_tpl) {
            char *items = strdup("");
            for (size_t i = 0; items && i < count; i++) {
                if (!langs[i].code) continue;
                const char *is_active = strcmp(langs[i].code, active) == 0
                    ? " boat-rudder__navbar__lang__item--active" : "";
                char *item = render_template(item_tpl, is_active, langs[i].code, return_enc,
                                              language_catalog_native(langs[i].code));
                items = item ? str_append(items, item) : NULL;
                free(item);
            }
            if (items) result = render_template(tpl, active_abbr, items);
            free(items);
            free(item_tpl);
        }
    } else {
        result = render_template(tpl, return_enc, active_abbr);
    }

    free(tpl);
    cms_languages_free(langs, count);
    return result ? result : strdup("");
}

// A theme switcher for the nav bar, "similar a la lista de idiomas" -
// mirrors language_selector() exactly: epoch 3 renders a drop-down whose
// entries link straight to /theme/set (sets the `theme` cookie, bounces
// back); epoch 1/2, where a drop-down cannot be relied on, get a plain
// link to the /theme page instead (theme_page.c), which itself mirrors
// /language's own epoch 1 (direct "<page>?theme=xx" link, no redirect -
// see theme_page.c) vs. epoch 2 (/theme/set) split. Epoch -1/0 still don't
// offer this: their look barely differs between themes today (colors
// already come from the same DB-backed palette regardless of which
// theme's markup is loaded - see theme-system-plan.md), so a page for a
// control that changes almost nothing there isn't worth it yet.
//
// Returns a malloc'd string ("" when there is nothing to offer: fewer than
// two themes, epoch -1/0, or the templates are missing), never NULL
// unless allocation fails.
static char *theme_selector(int epoch) {
    if (epoch < EPOCH_EARLY) return strdup("");

    char **keys = NULL;
    size_t count = 0;
    list_theme_keys(&keys, &count);

    // A single theme offers no choice; showing the control would be noise.
    if (count < 2) {
        free_theme_keys(keys, count);
        return strdup("");
    }

    const char *active = request_theme();

    char return_enc[1024];
    url_encode(return_enc, request_path(), sizeof(return_enc));

    char *tpl_path = generate_url_theme("menu/menu-theme_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) {
        free_theme_keys(keys, count);
        return strdup("");
    }

    char *result = NULL;
    if (epoch >= EPOCH_MODERN) {
        char *item_path = generate_url_theme("menu/menu-theme-item_epoch%d.html", epoch);
        char *item_tpl = item_path ? read_file_to_string(item_path) : NULL;
        free(item_path);

        if (item_tpl) {
            char *items = strdup("");
            for (size_t i = 0; items && i < count; i++) {
                const char *is_active = strcmp(keys[i], active) == 0
                    ? " boat-rudder__navbar__theme__item--active" : "";
                char *item = render_template(item_tpl, is_active, keys[i], return_enc, keys[i]);
                items = item ? str_append(items, item) : NULL;
                free(item);
            }
            if (items) result = render_template(tpl, active, items);
            free(items);
            free(item_tpl);
        }
    } else {
        // menu-theme_epoch{1,2}.html link to /theme?return=..., a full page
        // listing every theme (theme_page.c) - same shape as
        // menu-lang_epoch{1,2}.html linking to /language.
        result = render_template(tpl, return_enc, active);
    }

    free(tpl);
    free_theme_keys(keys, count);
    return result ? result : strdup("");
}

// Epoch 3 only: a link to /dashboard carrying the signed-in user's display
// name. request_user_name() (per-thread, resolved once per request in
// http_router.c) is "" whenever there is no active session, which is what
// keeps this link out of the navbar entirely for every visitor who isn't
// logged in.
//
// Rendered twice, from two different one-`%s` templates sharing the same
// name: menu-user_epoch3.html sits in the fixed top-left corner (desktop)
// and menu-user-mobile_epoch3.html rides inside the hamburger dropdown as
// one more boat-rudder__navbar__menu_item (mobile) - styles_epoch3.css
// swaps which one is visible at the 800px breakpoint, since a signed-in
// admin still needs a way to reach /dashboard once the corner link is
// hidden. See user_menu_item()/user_menu_item_mobile() below.
//
// Returns a malloc'd string ("" when logged out, older epochs, or the
// template is missing), never NULL unless allocation fails.
static char *user_link(int epoch, const char *subpath_fmt) {
    const char *name = request_user_name();
    if (epoch < EPOCH_MODERN || !name[0]) return strdup("");

    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    if (!tpl) return strdup("");

    char *result = render_template(tpl, name);
    free(tpl);
    return result ? result : strdup("");
}

static char *user_menu_item(int epoch) {
    return user_link(epoch, "menu/menu-user_epoch%d.html");
}

static char *user_menu_item_mobile(int epoch) {
    return user_link(epoch, "menu/menu-user-mobile_epoch%d.html");
}

char *menu(const char *current_url, int epoch) {
    char *menu_item_path    = generate_url_theme("menu/menu-item_epoch%d.html", epoch);
    char *selected_item_path = generate_url_theme("menu/menu-item-selected_epoch%d.html", epoch);
    char *separator_path    = generate_url_theme("menu/menu-item-separator_epoch%d.html", epoch);
    char *menu_path         = generate_url_theme("menu/menu_epoch%d.html", epoch);

    char *menu_item_tpl     = menu_item_path     ? read_file_to_string(menu_item_path)     : NULL;
    char *selected_item_tpl = selected_item_path ? read_file_to_string(selected_item_path) : NULL;
    char *separator         = separator_path     ? read_file_to_string(separator_path)     : NULL;
    char *menu_tpl          = menu_path          ? read_file_to_string(menu_path)          : NULL;

    free(menu_item_path);
    free(selected_item_path);
    free(separator_path);
    free(menu_path);

    char *items  = NULL;
    char *result = NULL;

    if (!menu_item_tpl || !separator || !menu_tpl) goto cleanup;
    if (!selected_item_tpl) selected_item_tpl = strdup(menu_item_tpl);

    CmsMenuItem *db_items = NULL;
    size_t db_count = 0;
    if (mongodb_manager_is_ready())
        cms_get_menu_items(request_lang(), &db_items, &db_count);

    const CmsMenuItem *menu_items = db_count > 0 ? db_items : FALLBACK_ITEMS;
    size_t item_count = db_count > 0 ? db_count : FALLBACK_ITEM_COUNT;

    items = strdup("");
    if (!items) {
        cms_menu_free(db_items, db_count);
        goto cleanup;
    }

    for (size_t i = 0; i < item_count; i++) {
        const char *sep = (i + 1 < item_count) ? separator : "";

        int is_selected = current_url && strcmp(current_url, menu_items[i].link) == 0;
        const char *tpl = is_selected ? selected_item_tpl : menu_item_tpl;
        char *item = render_template(tpl, menu_items[i].link, menu_items[i].name, sep);
        if (!item) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }

        items = str_append(items, item);
        free(item);
        if (!items) {
            cms_menu_free(db_items, db_count);
            goto cleanup;
        }
    }

    cms_menu_free(db_items, db_count);

    // Home banner stands in for the logo on the home page, so the menu only
    // carries it elsewhere - the reader still needs a way back. Mirrors the
    // previous site. Decided from the request path, not `current_url`: the
    // latter is the menu *section* and is hardcoded to "/" by
    // buildPageWebSite(), which every login/dashboard/language page goes
    // through - they would all look like home.
    char *logo = strdup("");
    int at_home = strcmp(request_path(), "/") == 0;
    if ((epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE || epoch == EPOCH_WML) && !at_home) {
        char *logo_path = generate_url_theme("menu/menu-logo_epoch%d.html", epoch);
        char *logo_tpl  = logo_path ? read_file_to_string(logo_path) : NULL;
        free(logo_path);
        if (logo_tpl) {
            free(logo);
            logo = logo_tpl;
        }
    }

    char *lang_html = language_selector(epoch);
    // Epoch 3 draws its own title in the nav bar and takes no logo slot;
    // that title is the personalizable site name instead of the logo.
    if (lang_html) {
        if (epoch >= EPOCH_MODERN) {
            char *site_name = cms_get_site_name();
            char *user_html = user_menu_item(epoch);
            char *user_html_mobile = user_menu_item_mobile(epoch);

            if (user_html_mobile) items = str_append(items, user_html_mobile);
            else { free(items); items = NULL; }
            free(user_html_mobile);

            char *theme_html = theme_selector(epoch);

            result = (site_name && user_html && items && theme_html)
                ? render_template(menu_tpl, user_html, site_name, items, theme_html, lang_html)
                : NULL;
            free(site_name);
            free(user_html);
            free(theme_html);
        } else {
            // No new %s slot on menu_epoch{1,2}.html's own container for
            // this - menu-theme_epoch{1,2}.html is a self-contained,
            // already-spaced snippet (mirrors menu-lang_epoch{1,2}.html's
            // own leading spacer/cell), so it is simply appended onto the
            // same lang_html argument the container already takes,
            // reassigning lang_html so the one free() below still covers
            // whichever buffer ends up here.
            char *theme_html = theme_selector(epoch);
            lang_html = theme_html ? str_append(lang_html, theme_html) : NULL;
            free(theme_html);
            result = lang_html ? render_template(menu_tpl, logo, items, lang_html) : NULL;
        }
    }
    free(lang_html);
    free(logo);

cleanup:
    free(menu_item_tpl);
    free(selected_item_tpl);
    free(separator);
    free(menu_tpl);
    free(items);
    return result;
}
