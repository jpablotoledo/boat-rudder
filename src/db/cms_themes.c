#include "cms_themes.h"
#include "mongodb_manager.h"
#include "../utils/read_file.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

#define THEMES_COLLECTION "themes"

// Every theme's own hardcoded styles_epoch3.css values, used as the
// fallback when that theme has no saved `themes.<key>.colors` document.
// This is keyed by theme, not a single global default: a theme with no
// entry here (or no document in Mongo) still needs *some* fallback for
// epoch 1/2's plain hex substitution (unlike epoch 3, which can lean on
// its own CSS var(--x, <fallback>) without any value from here at all -
// see splice_theme_colors() in page_layout.c), so dark's palette is the
// catch-all for a theme this table doesn't know about. Adding a theme's
// own entry here is optional - its own styles_epoch3.css hardcoded
// defaults already cover epoch 3; this table only matters for epoch 1/2
// looking right before anyone visits /dashboard/settings/themes.
typedef struct {
    const char *key;
    CmsThemeColors colors;
} ThemeDefaultEntry;

static const ThemeDefaultEntry THEME_DEFAULTS[] = {
    // Values from the project's Figma file, "Color palette Dark" variable
    // collection.
    {
        "dark",
        {
            .navbar_background         = "#241144",
            .navbar_menu_normal        = "#ffffff",
            .navbar_menu_hover         = "#98ffdd",
            .navbar_menu_active        = "#00ffab",
            .navbar_logo               = "#ffffff",
            .body_background           = "#241144",
            .home_content_background   = "#170c29",
            .home_content_text         = "#ffffff",
            .blog_list_item_background = "#000000",
            .blog_list_item_border     = "#00ffab",
            .blog_list_item_author     = "#dfd106",
            .blog_list_item_categories = "#98ffdd",
            .blog_list_item_date       = "#e68e4e",
        },
    },
    // Values from the project's Figma file, "Color palette Light" variable
    // collection.
    {
        "light",
        {
            .navbar_background         = "#8dd3ff",
            .navbar_menu_normal        = "#680072",
            .navbar_menu_hover         = "#006d49",
            .navbar_menu_active        = "#3800aa",
            .navbar_logo               = "#680072",
            .body_background           = "#8dd3ff",
            .home_content_background   = "#cbebff",
            .home_content_text         = "#000000",
            .blog_list_item_background = "#ffffff",
            .blog_list_item_border     = "#0009ac",
            .blog_list_item_author     = "#cb5600",
            .blog_list_item_categories = "#0076c0",
            .blog_list_item_date       = "#00636a",
        },
    },
};

#define THEME_DEFAULTS_COUNT (sizeof(THEME_DEFAULTS) / sizeof(THEME_DEFAULTS[0]))

static const CmsThemeColors *default_colors_for(const char *key) {
    for (size_t i = 0; i < THEME_DEFAULTS_COUNT; i++) {
        if (key && strcmp(THEME_DEFAULTS[i].key, key) == 0) return &THEME_DEFAULTS[i].colors;
    }
    return &THEME_DEFAULTS[0].colors; // dark - the catch-all, see comment above
}

// BSON can't hold a field starting with '-', so epoch -1 gets a spelled-out
// key; every other epoch's key mirrors its on-disk filename suffix.
// Ported from cms_site_settings.c (cms_get_site_banner()/cms_get_site_footer())
// when banner/footer moved here - see theme-scoped-personalization-plan.md.
static const char *epoch_field_name(int epoch) {
    switch (epoch) {
        case -1: return "epoch_neg1";
        case 0:  return "epoch0";
        case 1:  return "epoch1";
        case 2:  return "epoch2";
        case 3:  return "epoch3";
        default: return NULL;
    }
}

// Like bson_lang.c's resolve_lang_map(), but for a plain map<string,string>
// with no language fallback: returns `parent.field.key`, or "" if any of
// `field`, `key`, or `parent` itself is absent/not a string.
static char *nested_string_field(const bson_t *parent, const char *field, const char *key) {
    bson_iter_t iter, sub_iter;

    if (!bson_iter_init_find(&iter, parent, field) || !BSON_ITER_HOLDS_DOCUMENT(&iter))
        return strdup("");

    uint32_t len;
    const uint8_t *data;
    bson_iter_document(&iter, &len, &data);

    bson_t sub;
    bson_init_static(&sub, data, len);

    if (bson_iter_init_find(&sub_iter, &sub, key) && BSON_ITER_HOLDS_UTF8(&sub_iter))
        return strdup(bson_iter_utf8(&sub_iter, NULL));

    return strdup("");
}

// Reads html/themes/<key>/<subpath_fmt % epoch> directly, keyed by the
// `key` this function was given - deliberately NOT generate_url_theme(),
// which resolves against request_theme() (the *active* request's theme)
// regardless of any key passed elsewhere. That distinction only matters
// when a caller's `key` might differ from request_theme() (an admin
// editing a theme that isn't their own session's active one); reading the
// path straight from `key` keeps cms_get_theme_banner()/_footer() correct
// under that call too, not just the render path's key = request_theme()
// case. No templates/ fallback needed here: mainbanner/ and layout/ are
// always theme-owned (theme-system-plan.md §2), never shared.
static char *load_theme_owned_file(const char *key, const char *subpath_fmt, int epoch) {
    char subpath[128];
    if (snprintf(subpath, sizeof(subpath), subpath_fmt, epoch) < 0) return strdup("");

    char path[256];
    if (snprintf(path, sizeof(path), "./html/themes/%s/%s", key, subpath) < 0) return strdup("");

    char *body = read_file_to_string(path);
    return body ? body : strdup("");
}

static void copy_field(const bson_t *doc, const char *field, char *out, size_t out_size) {
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, doc, field) && BSON_ITER_HOLDS_UTF8(&iter)) {
        strncpy(out, bson_iter_utf8(&iter, NULL), out_size - 1);
        out[out_size - 1] = '\0';
    }
}

int cms_get_theme_colors(const char *key, CmsThemeColors *out) {
    *out = *default_colors_for(key);
    if (!key || !key[0]) return 1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return 1;

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "colors") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            uint32_t len;
            const uint8_t *data;
            bson_iter_document(&iter, &len, &data);

            bson_t colors;
            bson_init_static(&colors, data, len);

            copy_field(&colors, "navbar-background", out->navbar_background, sizeof(out->navbar_background));
            copy_field(&colors, "navbar-menu-normal", out->navbar_menu_normal, sizeof(out->navbar_menu_normal));
            copy_field(&colors, "navbar-menu-hover", out->navbar_menu_hover, sizeof(out->navbar_menu_hover));
            copy_field(&colors, "navbar-menu-active", out->navbar_menu_active, sizeof(out->navbar_menu_active));
            copy_field(&colors, "navbar-logo", out->navbar_logo, sizeof(out->navbar_logo));
            copy_field(&colors, "body-background", out->body_background, sizeof(out->body_background));
            copy_field(&colors, "home-content-background", out->home_content_background, sizeof(out->home_content_background));
            copy_field(&colors, "home-content-text", out->home_content_text, sizeof(out->home_content_text));
            copy_field(&colors, "blog-list-item-background", out->blog_list_item_background, sizeof(out->blog_list_item_background));
            copy_field(&colors, "blog-list-item-border", out->blog_list_item_border, sizeof(out->blog_list_item_border));
            copy_field(&colors, "blog-list-item-author", out->blog_list_item_author, sizeof(out->blog_list_item_author));
            copy_field(&colors, "blog-list-item-categories", out->blog_list_item_categories, sizeof(out->blog_list_item_categories));
            copy_field(&colors, "blog-list-item-date", out->blog_list_item_date, sizeof(out->blog_list_item_date));
        }
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_theme_colors: cursor error: %s", error.message);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return 1;
}

int cms_update_theme_colors(const char *key, const CmsThemeColors *colors) {
    if (!key || !key[0]) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return -1;

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    bson_t *update = BCON_NEW(
        "$set", "{",
            "key", BCON_UTF8(key),
            "colors", "{",
                "navbar-background", BCON_UTF8(colors->navbar_background),
                "navbar-menu-normal", BCON_UTF8(colors->navbar_menu_normal),
                "navbar-menu-hover", BCON_UTF8(colors->navbar_menu_hover),
                "navbar-menu-active", BCON_UTF8(colors->navbar_menu_active),
                "navbar-logo", BCON_UTF8(colors->navbar_logo),
                "body-background", BCON_UTF8(colors->body_background),
                "home-content-background", BCON_UTF8(colors->home_content_background),
                "home-content-text", BCON_UTF8(colors->home_content_text),
                "blog-list-item-background", BCON_UTF8(colors->blog_list_item_background),
                "blog-list-item-border", BCON_UTF8(colors->blog_list_item_border),
                "blog-list-item-author", BCON_UTF8(colors->blog_list_item_author),
                "blog-list-item-categories", BCON_UTF8(colors->blog_list_item_categories),
                "blog-list-item-date", BCON_UTF8(colors->blog_list_item_date),
            "}",
        "}"
    );
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_theme_colors: update failed: %s", error.message);

    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

// Shared by cms_get_theme_banner()/cms_get_theme_footer(): the stored
// value for `key`.`top_field`.<epoch field>, or "" if unset/unreachable -
// the *stored* value only, no file fallback (callers add that themselves).
static char *stored_epoch_field(const char *key, const char *top_field, int epoch) {
    const char *field = epoch_field_name(epoch);
    if (!key || !key[0] || !field) return strdup("");

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return strdup("");

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc))
        result = nested_string_field(doc, top_field, field);

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup("");
}

char *cms_get_theme_banner(const char *key, int epoch) {
    if (epoch_to_index(epoch) < 0) return strdup("");

    char *db_value = stored_epoch_field(key, "banner_html", epoch);
    if (db_value[0]) return db_value;
    free(db_value);
    return load_theme_owned_file(key, "mainbanner/mainbanner_epoch%d.html", epoch);
}

char *cms_get_theme_footer(const char *key, int epoch) {
    if (epoch_to_index(epoch) < 0) return strdup("");

    char *db_value = stored_epoch_field(key, "footer_html", epoch);
    if (db_value[0]) return db_value;
    free(db_value);
    return load_theme_owned_file(key, "layout/footer_epoch%d.html", epoch);
}

static void get_stored_values(const char *key, const char *top_field, char *out_values[EPOCH_COUNT]) {
    for (int epoch = -1; epoch <= 3; epoch++)
        out_values[epoch_to_index(epoch)] = stored_epoch_field(key, top_field, epoch);
}

void cms_get_theme_banner_values(const char *key, char *out_values[EPOCH_COUNT]) {
    get_stored_values(key, "banner_html", out_values);
}

void cms_get_theme_footer_values(const char *key, char *out_values[EPOCH_COUNT]) {
    get_stored_values(key, "footer_html", out_values);
}

// Shared by cms_update_theme_banner()/cms_update_theme_footer(): $set
// themes.<key>.<top_field>.<epoch field> = html.
static int update_theme_epoch_field(const char *key, const char *top_field, int epoch,
                                     const char *html) {
    const char *field = epoch_field_name(epoch);
    if (!key || !key[0] || !field) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(THEMES_COLLECTION);
    if (!collection) return -1;

    char dotted[64];
    snprintf(dotted, sizeof(dotted), "%s.%s", top_field, field);

    bson_t *query = BCON_NEW("key", BCON_UTF8(key));
    bson_t *update = bson_new();
    bson_t set_doc;
    bson_append_document_begin(update, "$set", -1, &set_doc);
    bson_append_utf8(&set_doc, "key", -1, key, -1);
    bson_append_utf8(&set_doc, dotted, -1, html, -1);
    bson_append_document_end(update, &set_doc);
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_update_theme_%s: update failed: %s", top_field, error.message);

    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_update_theme_banner(const char *key, int epoch, const char *html) {
    return update_theme_epoch_field(key, "banner_html", epoch, html);
}

int cms_update_theme_footer(const char *key, int epoch, const char *html) {
    return update_theme_epoch_field(key, "footer_html", epoch, html);
}
