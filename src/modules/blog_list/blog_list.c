#include "blog_list.h"
#include "../../utils/category_tags.h"
#include "../../db/cms_entries.h"
#include "../../db/cms_themes.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <stdlib.h>
#include <string.h>

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

static char *render_item(const CmsBlogListItem *item, const char *item_tpl, int epoch) {
    char *categories_html = category_tags_render(item->category_links, item->category_names,
                                                  item->category_count, epoch);
    if (!categories_html) return NULL;

    char *result;
    if (epoch >= EPOCH_EARLY) {
        char *link_url = render_template("/blog/%s", item->link);
        // Epoch 1 predates progressive JPEG, so it gets the GIF the optimizer
        // writes - same variant and rewrite as image_for_epoch()/gallery_thumb().
        char *thumb = (epoch == EPOCH_EARLY)
            ? image_url_variant(item->header_image_url, "_micro")
            : image_url_variant(item->header_image_url, "_small");
        if (epoch == EPOCH_EARLY && thumb) {
            char *dot = strrchr(thumb, '.');
            if (dot) strcpy(dot, ".gif");
        }

        if (epoch == EPOCH_MODERN) {
            // home-blog-item_epoch3.html carries its own color via CSS
            // classes (boat-rudder__home-blog__item__byline-owner etc.) -
            // no color arguments to insert. Unlike epoch 2's item template,
            // this one addresses its %s slots sequentially, not by %N$s
            // position, so passing the epoch 1/2 color arguments here would
            // shift every slot after them - not merely "extra, ignored"
            // varargs - and print a hex string where the author name goes.
            result = (link_url && thumb)
                ? render_template(item_tpl, thumb, link_url, item->header_title,
                                   item->header_summary,
                                   item->header_hide_author ? "" : item->header_author,
                                   categories_html, item->header_date)
                : NULL;
        } else {
            // Epoch 1/2 have no CSS custom properties (epoch 1: no CSS at
            // all; epoch 2's inline <style> predates CSS3 variables), so
            // the theme's colors go straight into <font color>/bordercolor
            // attributes as %s substitutions instead. See
            // develop_docs/plans/theme-system-plan.md's epoch 1/2 analysis.
            CmsThemeColors retro;
            cms_get_theme_colors(request_theme(), &retro);

            result = (link_url && thumb)
                ? render_template(item_tpl, thumb, link_url, item->header_title,
                                   item->header_summary,
                                   retro.blog_list_item_author,
                                   item->header_hide_author ? "" : item->header_author,
                                   categories_html,
                                   retro.blog_list_item_date, item->header_date,
                                   retro.blog_list_item_border)
                : NULL;
        }
        free(thumb);
        free(link_url);
    } else {
        // Text-only and WML, like render_image()'s epoch <= EPOCH_PRESTANDARD
        // branch: no thumbnail, but still a real link to the article and a
        // byline - a title with nothing to click and no author was just a
        // template that had never been given the arguments to draw them,
        // not a deliberate restriction for either epoch.
        char *link_url = render_template("/blog/%s", item->link);
        result = link_url
            ? render_template(item_tpl, link_url, item->header_title,
                               item->header_summary,
                               item->header_hide_author ? "" : item->header_author,
                               categories_html, item->header_date)
            : NULL;
        free(link_url);
    }

    free(categories_html);
    return result;
}

// The one implementation. `heading` is printed above the list; `limit` caps
// the query; `category_id_hex` filters it when non-NULL.
static char *render_list(int epoch, const char *lang, const char *heading,
                          int limit, const char *category_id_hex) {
    char *item_path    = generate_url_theme("home-blog/home-blog-item_epoch%d.html", epoch);
    char *content_path = generate_url_theme("home-blog/home-blog_epoch%d.html", epoch);

    char *item_tpl    = item_path    ? read_file_to_string(item_path)    : NULL;
    char *content_tpl = content_path ? read_file_to_string(content_path) : NULL;

    free(item_path);
    free(content_path);

    char *items  = NULL;
    char *result = NULL;

    CmsBlogListItem *entries     = NULL;
    size_t           entry_count = 0;

    if (!item_tpl || !content_tpl) goto cleanup;

    if (category_id_hex)
        cms_get_blog_entries_by_category(lang, limit, category_id_hex, &entries, &entry_count);
    else
        cms_get_blog_entries(lang, limit, &entries, &entry_count);

    if (entry_count == 0) {
        items = load_template("home-blog/empty_epoch%d.html", epoch);
    } else {
        items = strdup("");
        for (size_t i = 0; items && i < entry_count; i++) {
            char *item = render_item(&entries[i], item_tpl, epoch);
            items = item ? str_append(items, item) : NULL;
            free(item);
        }
    }

    if (items) result = render_template(content_tpl, heading, items);

cleanup:
    cms_blog_list_free(entries, entry_count);
    free(item_tpl);
    free(content_tpl);
    free(items);
    return result;
}

char *home_blog(int epoch, const char *lang) {
    return render_list(epoch, lang, "Latest Blog Posts", HOME_BLOG_LIMIT, NULL);
}

char *blog_list(int epoch, const char *lang) {
    return render_list(epoch, lang, "Blog", BLOG_LIST_LIMIT, NULL);
}

char *blog_list_category(int epoch, const char *lang, const char *category_id_hex) {
    return render_list(epoch, lang, "Blog", BLOG_LIST_LIMIT, category_id_hex);
}
