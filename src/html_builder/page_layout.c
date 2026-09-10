#include "page_layout.h"
#include "../db/cms_themes.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/request_theme.h"
#include "../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Replaces `marker` with the contents of layout/<part>_epoch<N>.html. A part
// that has no file for this epoch resolves to nothing, which is how the retro
// epochs drop the epoch-3 viewers without needing an empty file each.
static char *splice_part(char *html, const char *marker, const char *part, int epoch) {
    if (!html || !strstr(html, marker)) return html;

    char subpath[128];
    snprintf(subpath, sizeof(subpath), "layout/%s_epoch%%d.html", part);

    char *path = generate_url_theme(subpath, epoch);
    char *body = path ? read_file_to_string(path) : NULL;
    free(path);

    char *result = str_replace_first(html, marker, body ? body : "");
    free(body);
    free(html);
    return result;
}

// Like splice_part(), but for the footer specifically: its content is
// personalizable per theme from /dashboard/settings/themes/<key>/footer,
// so it comes from cms_get_theme_footer() (DB override for the active
// theme, falling back to that same theme's on-disk
// layout/footer_epoch<N>.html) instead of a direct file read.
static char *splice_footer(char *html, int epoch) {
    if (!html || !strstr(html, "{{FOOTER}}")) return html;

    char *body = cms_get_theme_footer(request_theme(), epoch);
    char *result = str_replace_first(html, "{{FOOTER}}", body ? body : "");
    free(body);
    free(html);
    return result;
}

// Replaces {{THEME_COLORS}} (epoch 3's layout only - older epochs have no
// color model, and simply lack the marker, so this is a no-op there) with a
// small inline <style>:root{...}</style> fragment carrying the active
// theme's DB-editable color tokens (/dashboard/settings/themes) - the same
// one shared palette every epoch reads from, per cms_themes.h. Every
// styles_epoch3.css rule that opts in reads its value via
// var(--br-color-x, <hardcoded-default>), so a theme with no saved colors
// renders identically whether or not this marker even exists.
static char *splice_theme_colors(char *html) {
    if (!html || !strstr(html, "{{THEME_COLORS}}")) return html;

    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    char *style = render_template(
        "<style>:root{"
        "--br-color-navbar-background:%s;--br-color-navbar-menu-normal:%s;"
        "--br-color-navbar-menu-hover:%s;--br-color-navbar-menu-active:%s;"
        "--br-color-navbar-logo:%s;--br-color-body-background:%s;"
        "--br-color-home-content-background:%s;--br-color-home-content-text:%s;"
        "--br-color-blog-list-item-background:%s;--br-color-blog-list-item-border:%s;"
        "--br-color-blog-list-item-author:%s;--br-color-blog-list-item-categories:%s;"
        "--br-color-blog-list-item-date:%s;"
        "}</style>",
        colors.navbar_background, colors.navbar_menu_normal,
        colors.navbar_menu_hover, colors.navbar_menu_active,
        colors.navbar_logo, colors.body_background,
        colors.home_content_background, colors.home_content_text,
        colors.blog_list_item_background, colors.blog_list_item_border,
        colors.blog_list_item_author, colors.blog_list_item_categories,
        colors.blog_list_item_date);

    char *result = str_replace_first(html, "{{THEME_COLORS}}", style ? style : "");
    free(style);
    free(html);
    return result;
}

// Replaces {{COLOR_BACKGROUND}}/{{COLOR_TEXT}}/{{COLOR_ACCENT}} (epoch 1/2's
// layout only - epoch 3 has its own {{THEME_COLORS}} CSS block above, and
// epoch -1/0 have no color model, so those layouts simply lack these
// markers and this is a no-op) with plain hex values from the *same*
// cms_get_theme_colors() epoch 3 reads - one shared palette, not a
// separate one per epoch (see cms_themes.h). Epoch 1/2 have no CSS custom
// properties (epoch 1: no CSS at all; epoch 2's inline <style> predates
// CSS3 variables), so colors go straight into HTML attributes
// (bgcolor/text/link/vlink) as substituted text, not injected CSS. There
// is no epoch 1/2 concept of "hover"/"active" menu states, so
// {{COLOR_ACCENT}} (link/vlink - appears twice per layout, hence two
// passes below) uses navbar-menu-normal, the closest of the 13 tokens to
// a generic link color; {{COLOR_BACKGROUND}}/{{COLOR_TEXT}} use
// body-background/home-content-text (body text has no dedicated token of
// its own in the Figma palette - home-content-text is the closest match,
// being the main visible text color on these epochs' pages).
static char *splice_retro_colors(char *html) {
    if (!html) return NULL;
    if (!strstr(html, "{{COLOR_BACKGROUND}}") && !strstr(html, "{{COLOR_TEXT}}") &&
        !strstr(html, "{{COLOR_ACCENT}}"))
        return html;

    CmsThemeColors colors;
    cms_get_theme_colors(request_theme(), &colors);

    char *step = str_replace_first(html, "{{COLOR_BACKGROUND}}", colors.body_background);
    free(html);
    if (!step) return NULL;

    char *next = str_replace_first(step, "{{COLOR_TEXT}}", colors.home_content_text);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_first(step, "{{COLOR_ACCENT}}", colors.navbar_menu_normal);
    free(step);
    if (!next) return NULL;
    step = next;

    next = str_replace_first(step, "{{COLOR_ACCENT}}", colors.navbar_menu_normal);
    free(step);
    return next;
}

char *page_layout_wrap(char *fragment_html, const char *page_title, int epoch,
                       const char *body_background) {
    if (!fragment_html) return NULL;

    fragment_html = splice_footer(fragment_html, epoch);
    fragment_html = splice_part(fragment_html, "{{LIGHTBOX}}",   "lightbox",   epoch);
    fragment_html = splice_part(fragment_html, "{{HOME-MODAL}}", "home-modal", epoch);
    if (!fragment_html) return NULL;

    char *path   = generate_url_theme("layout/layout_epoch%d.html", epoch);
    char *layout = path ? read_file_to_string(path) : NULL;
    free(path);
    if (!layout) {
        free(fragment_html);
        return NULL;
    }

    char *title_tag = build_title_tag(page_title);
    char *titled    = title_tag ? str_replace_first(layout, "{{PAGE_TITLE}}", title_tag) : NULL;
    free(title_tag);
    free(layout);
    if (!titled) {
        free(fragment_html);
        return NULL;
    }

    char *with_bg = str_replace_first(titled, "{{BODY_BACKGROUND}}",
                                      body_background ? body_background : "");
    free(titled);
    if (!with_bg) {
        free(fragment_html);
        return NULL;
    }

    char *with_retro_colors = splice_retro_colors(with_bg);
    if (!with_retro_colors) {
        free(fragment_html);
        return NULL;
    }

    char *with_colors = splice_theme_colors(with_retro_colors);
    if (!with_colors) {
        free(fragment_html);
        return NULL;
    }

    char *result = str_replace_first(with_colors, "{{CONTENT}}", fragment_html);
    free(with_colors);
    free(fragment_html);
    return result;
}
