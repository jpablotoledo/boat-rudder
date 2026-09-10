#include "category_tags.h"
#include "detect_epoch.h"
#include "generate_url_theme.h"
#include "read_file.h"
#include "request_theme.h"
#include "template_utils.h"
#include "../db/cms_themes.h"
#include <stdlib.h>
#include <string.h>

static char *load_part(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

char *category_tags_render(char **links, char **names, size_t count, int epoch) {
    if (count == 0 || !names) return strdup("");

    char *item_tpl = load_part("elements/category/category_epoch%d.html", epoch);
    if (!item_tpl) return strdup("");

    // Every epoch links its tags. An anchor is the one thing all of them can
    // do - WML included - so a category tag is clickable wherever it appears.
    int linked = links != NULL;

    char *sep_tpl = load_part("elements/category/category-separator_epoch%d.html", epoch);

    // Epoch 1/2 have no CSS custom properties (see
    // develop_docs/plans/theme-system-plan.md's epoch 1/2 analysis), so the
    // category tag color - today hardcoded in category_epoch{1,2}.html -
    // becomes a %s substituted straight into the <font color>/style
    // attribute instead. Epoch 3 already carries its own color via the
    // boat-rudder__entry-category CSS class; -1/0 have no color model.
    int needs_color = (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE);
    CmsThemeColors retro;
    if (needs_color) cms_get_theme_colors(request_theme(), &retro);

    char *result = strdup("");
    for (size_t i = 0; result && i < count; i++) {
        char *tag;
        if (needs_color)
            tag = linked ? render_template(item_tpl, links[i], retro.blog_list_item_categories, names[i])
                         : render_template(item_tpl, retro.blog_list_item_categories, names[i]);
        else
            tag = linked ? render_template(item_tpl, links[i], names[i])
                         : render_template(item_tpl, names[i]);
        if (sep_tpl && i > 0) result = str_append(result, sep_tpl);
        if (result && tag) result = str_append(result, tag);
        free(tag);
    }

    free(sep_tpl);
    free(item_tpl);
    return result;
}
