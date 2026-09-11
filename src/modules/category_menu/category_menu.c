#include "category_menu.h"
#include "../../db/cms_themes.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

char *category_menu_render(const CmsCategoryItem *categories, size_t count,
                            const char *current_slug, int epoch) {
    if (count == 0) return strdup("");

    char *path = generate_url_theme("category-menu/category-menu_epoch%d.html", epoch);
    char *container_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    path = generate_url_theme("category-menu/category-menu-item_epoch%d.html", epoch);
    char *item_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    path = generate_url_theme("category-menu/category-menu-item-selected_epoch%d.html", epoch);
    char *selected_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    // What goes *between* two items, never before the first or after the last.
    // Epochs that separate the items by layout - a row of <td> in epoch 1, a
    // flex container in epoch 3 - hold nothing but whitespace here.
    path = generate_url_theme("category-menu/category-menu-separator_epoch%d.html", epoch);
    char *separator_tpl = path ? read_file_to_string(path) : NULL;
    free(path);

    if (!container_tpl || !item_tpl) {
        free(container_tpl); free(item_tpl); free(selected_tpl); free(separator_tpl);
        return strdup("");
    }
    if (!selected_tpl) selected_tpl = strdup(item_tpl);

    // No stylesheet on epoch 1/2, so these colors have to be real <font
    // color> attributes rather than CSS classes nothing defines - matches
    // the navbar menu's own colors (menu.c), which is what this menu sits
    // right below on /blog. Epoch 3 already gets them as CSS vars (see
    // .boat-rudder__navbar__category_item in styles_epoch3.css).
    int needs_color = (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE);
    CmsThemeColors colors;
    if (needs_color) cms_get_theme_colors(request_theme(), &colors);

    char *items = strdup("");
    for (size_t i = 0; items && i < count; i++) {
        char *slug = slugify(categories[i].name);
        int is_selected = (current_slug && slug && strcmp(slug, current_slug) == 0);
        const char *tpl = is_selected ? selected_tpl : item_tpl;
        char *item = needs_color
            ? render_template(tpl, slug ? slug : "",
                               is_selected ? colors.navbar_menu_active : colors.navbar_menu_normal,
                               categories[i].name)
            : render_template(tpl, slug ? slug : "", categories[i].name);
        free(slug);
        if (separator_tpl && i > 0) items = str_append(items, separator_tpl);
        if (items && item) items = str_append(items, item);
        free(item);
    }

    char *result = NULL;
    if (items) {
        result = (epoch == EPOCH_MIDDLE)
            ? render_template(container_tpl, colors.navbar_background, colors.navbar_background, items)
            : render_template(container_tpl, items);
    }
    free(items);
    free(container_tpl); free(item_tpl); free(selected_tpl); free(separator_tpl);
    return result ? result : strdup("");
}
