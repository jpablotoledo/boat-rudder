#include "fonts_admin.h"
#include "../../db/cms_fonts.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
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

// One @font-face per uploaded font, so the table's Name column can preview
// each font in its own face (see fonts_admin_list()'s use below) - the same
// declaration shape as page_layout.c's build_logo_font_css(), just one
// <style> block covering every font at once instead of a single theme's
// pick.
static char *build_preview_style(const CmsFont *fonts, size_t count) {
    if (count == 0) return strdup("");

    char *css = strdup("<style>");
    for (size_t i = 0; css && i < count; i++) {
        char *rule = render_template(
            "@font-face{font-family:'%s';src:url('/assets/fonts/%s') format('%s');}",
            fonts[i].name, fonts[i].filename, cms_font_format_for_filename(fonts[i].filename));
        css = rule ? str_append(css, rule) : NULL;
        free(rule);
    }
    if (css) css = str_append(css, "</style>");
    return css ? css : strdup("");
}

char *fonts_admin_list(int epoch, const char *error_message) {
    char *list_tpl = load_template("dashboard/fonts/list_epoch%d.html", epoch);
    char *row_tpl  = load_template("dashboard/fonts/list-row_epoch%d.html", epoch);

    char *error_html = NULL;
    char *preview_style = NULL;
    char *rows        = NULL;
    char *result       = NULL;

    CmsFont *fonts = NULL;
    size_t count = 0;

    if (!list_tpl || !row_tpl) goto cleanup;

    if (error_message && error_message[0]) {
        char *error_tpl = load_template("dashboard/fonts/list-error_epoch%d.html", epoch);
        if (error_tpl) {
            error_html = render_template(error_tpl, error_message);
            free(error_tpl);
        }
    }

    cms_get_fonts(&fonts, &count);

    preview_style = build_preview_style(fonts, count);
    if (!preview_style) goto cleanup;

    rows = strdup("");
    for (size_t i = 0; rows && i < count; i++) {
        char *name = html_encode_alloc(fonts[i].name);
        char *filename = name ? html_encode_alloc(fonts[i].filename) : NULL;
        char *styled_name = filename
            ? render_template("<span style=\"font-family:'%s'\">%s</span>", name, name)
            : NULL;
        char *row = styled_name ? render_template(row_tpl, styled_name, filename, fonts[i].id) : NULL;
        free(name);
        free(filename);
        free(styled_name);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) goto cleanup;

    result = render_template(list_tpl, preview_style, error_html ? error_html : "", rows);

cleanup:
    cms_fonts_free(fonts, count);
    free(list_tpl);
    free(row_tpl);
    free(error_html);
    free(preview_style);
    free(rows);
    return result;
}
