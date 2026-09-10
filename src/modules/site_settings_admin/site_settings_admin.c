#include "site_settings_admin.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// html_encode() needs a caller-sized buffer; worst case every byte of `src`
// expands to "&quot;" (6 bytes), so 6x + 1 is always enough.
static char *html_encode_alloc(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    html_encode(out, src, cap);
    return out;
}

char *site_settings_general_page(int epoch, const char *site_name, const char *error_message) {
    char *page_tpl  = load_template("dashboard/settings/settings_epoch%d.html", epoch);
    char *error_tpl = load_template("dashboard/settings/settings-error_epoch%d.html", epoch);

    char *error_html = NULL;
    char *encoded_name = NULL;
    char *result = NULL;

    if (!page_tpl) goto cleanup;

    if (error_message && error_message[0] && error_tpl)
        error_html = render_template(error_tpl, error_message);

    encoded_name = html_encode_alloc(site_name ? site_name : "");
    if (!encoded_name) goto cleanup;

    result = render_template(page_tpl, error_html ? error_html : "", encoded_name);

cleanup:
    free(page_tpl);
    free(error_tpl);
    free(error_html);
    free(encoded_name);
    return result;
}

// Epoch (-1..3) -> a short human label for the admin form's <h2>. Index via
// epoch_to_index().
static const char *EPOCH_LABELS[EPOCH_COUNT] = {
    "Epoch -1 (WAP / WML)",
    "Epoch 0 (text browsers)",
    "Epoch 1 (early HTML, tables/font)",
    "Epoch 2 (HTML 3.2 tables)",
    "Epoch 3 (modern)",
};

// Shared by site_settings_banner_page()/site_settings_footer_page():
// `key` is the theme being edited (not necessarily the admin's own active
// theme - see theme-scoped-personalization-plan.md §4); `settings_segment`
// is the /dashboard/settings/themes/<key>/<segment>/<epoch> route
// ("banner"/"footer"); `asset_component` is the theme-assets directory name
// under html/themes/<key>/assets/ ("mainbanner"/"footer") - the two differ
// because the on-disk directory predates this feature and keeps its name.
static char *asset_page(int epoch, const char *title, const char *key, const char *settings_segment,
                         const char *asset_component, char *const values[EPOCH_COUNT]) {
    char *page_tpl  = load_template("dashboard/settings/settings-asset_epoch%d.html", epoch);
    char *panel_tpl = load_template("dashboard/settings/settings-asset-panel_epoch%d.html", epoch);

    char *panels = NULL;
    char *result = NULL;

    if (!page_tpl || !panel_tpl) goto cleanup;

    panels = strdup("");
    for (int e = -1; panels && e <= 3; e++) {
        int i = epoch_to_index(e);
        char epoch_str[4];
        snprintf(epoch_str, sizeof(epoch_str), "%d", e);

        char *encoded = html_encode_alloc(values[i] ? values[i] : "");
        if (!encoded) {
            free(panels);
            panels = NULL;
            break;
        }

        char *panel = render_template(panel_tpl, asset_component, epoch_str, key, EPOCH_LABELS[i],
                                       key, settings_segment, epoch_str, encoded);
        free(encoded);
        if (!panel) {
            free(panels);
            panels = NULL;
            break;
        }

        panels = str_append(panels, panel);
        free(panel);
    }
    if (!panels) goto cleanup;

    result = render_template(page_tpl, title, key, panels);

cleanup:
    free(page_tpl);
    free(panel_tpl);
    free(panels);
    return result;
}

char *site_settings_banner_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Home banner", key, "banner", "mainbanner", values);
}

char *site_settings_footer_page(int epoch, const char *key, char *const values[EPOCH_COUNT]) {
    return asset_page(epoch, "Footer", key, "footer", "footer", values);
}

char *site_settings_preview_page(int epoch) {
    return load_template("dashboard/settings/preview_epoch%d.html", epoch);
}

char *site_settings_themes_page(int epoch, const ThemeEntry *themes, size_t count) {
    char *page_tpl     = load_template("dashboard/settings/settings-themes_epoch%d.html", epoch);
    char *panel_tpl    = load_template("dashboard/settings/settings-themes-panel_epoch%d.html", epoch);
    char *activate_tpl = load_template("dashboard/settings/settings-themes-activate_epoch%d.html", epoch);

    char *panels = NULL;
    char *result = NULL;

    if (!page_tpl || !panel_tpl || !activate_tpl) goto cleanup;

    panels = strdup("");
    for (size_t i = 0; panels && i < count; i++) {
        char *activate = themes[i].active ? strdup("") : render_template(activate_tpl, themes[i].key);
        if (!activate) {
            free(panels);
            panels = NULL;
            break;
        }

        const CmsThemeColors *c = &themes[i].colors;
        char *panel = render_template(panel_tpl, themes[i].key,
                                       themes[i].active ? " (active)" : "", activate,
                                       themes[i].key,
                                       c->navbar_background, c->navbar_menu_normal,
                                       c->navbar_menu_hover, c->navbar_menu_active,
                                       c->navbar_logo,
                                       c->body_background,
                                       c->home_content_background, c->home_content_text,
                                       c->blog_list_item_background, c->blog_list_item_border,
                                       c->blog_list_item_author, c->blog_list_item_categories,
                                       c->blog_list_item_date,
                                       themes[i].key, themes[i].key);
        free(activate);
        if (!panel) {
            free(panels);
            panels = NULL;
            break;
        }

        panels = str_append(panels, panel);
        free(panel);
    }
    if (!panels) goto cleanup;

    result = render_template(page_tpl, panels);

cleanup:
    free(page_tpl);
    free(panel_tpl);
    free(activate_tpl);
    free(panels);
    return result;
}
