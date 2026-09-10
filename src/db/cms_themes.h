#ifndef CMS_THEMES_H
#define CMS_THEMES_H

#include "../utils/detect_epoch.h"

// A theme's DB-editable color tokens - the 13 variables defined in the
// project's Figma file ("Color palette Dark"/"Color palette Light"), one
// shared palette for every epoch that has a concept of color at all (1, 2
// and 3), not split per epoch. How each value actually reaches the page
// differs by epoch, since epoch 1/2 have no CSS custom properties (epoch 1:
// no CSS at all; epoch 2: a tiny inline <style> for typography only,
// predating CSS3 variables) - epoch 3 gets these as --br-color-<name> CSS
// variables (names hyphenated to match the Figma variable exactly, e.g.
// --br-color-navbar-background), epoch 1/2 get a handful of them (there is
// no epoch 1/2 concept of "hover", for instance) substituted straight into
// HTML attributes (bgcolor/text/link/vlink/bordercolor/<font color>) - see
// page_layout.c's splice_retro_colors() and blog_list.c/category_tags.c for
// exactly which. Unlike site_settings, `themes` holds one *sparse* document
// per theme key that an admin has actually customized from
// /dashboard/settings/themes - a theme with no document just keeps its own
// Figma-defined defaults (see cms_themes.c's THEME_DEFAULTS).
typedef struct {
    char navbar_background[8];          // "#rrggbb" - navbar container background
    char navbar_menu_normal[8];         // menu item link color, unvisited/no interaction
    char navbar_menu_hover[8];          // menu item link color, :hover
    char navbar_menu_active[8];         // menu item link color, current page
    char navbar_logo[8];                // site name/logo text color
    char body_background[8];            // page body background
    char home_content_background[8];    // "Welcome" home-content section background
    char home_content_text[8];          // home-content section text
    char blog_list_item_background[8];  // each blog listing card's background
    char blog_list_item_border[8];      // blog listing card border (epoch 1 has no box to border)
    char blog_list_item_author[8];      // blog listing card byline author name
    char blog_list_item_categories[8];  // category tag color/background
    char blog_list_item_date[8];        // blog listing card byline date
} CmsThemeColors;

// db.themes.findOne({key}). Fills `out` with the stored colors, or with
// epoch 3's own hardcoded styles_epoch3.css values (see cms_themes.c) if
// no document exists for `key` or mongodb is unreachable - a theme with no
// saved colors must render exactly as it does today. Every value applies
// identically to epoch 1/2/3 rendering (see the type's doc above), so
// there is only ever one default set, not one per epoch: epoch 1/2's
// fresh-install appearance already differs slightly from their own prior
// hardcoded colors as a result (they previously had their own distinct
// muted palette; unifying the settings means adopting epoch 3's) - the
// disclosed, accepted trade-off of "one control affects every epoch it
// can" instead of duplicating the settings per epoch. Always returns 1.
int cms_get_theme_colors(const char *key, CmsThemeColors *out);

// db.themes.updateOne({key}, {$set: {colors}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_theme_colors(const char *key, const CmsThemeColors *colors);

// Home banner and footer, one raw-markup value per epoch, now scoped to
// the theme they visually belong with instead of site-wide - see
// theme-scoped-personalization-plan.md. "" (unset) falls back to that same
// theme's own on-disk file (mainbanner/mainbanner_epoch<N>.html /
// layout/footer_epoch<N>.html via generate_url_theme()), so a theme with
// no saved banner/footer renders exactly as it does today.

// Returns a malloc'd string, never NULL: the DB value for `epoch` if
// non-empty, otherwise that theme's on-disk fallback. Used by
// mainbanner()/page_layout_wrap() with key = request_theme(), so a
// banner/footer follows the same per-visitor precedence colors already do.
char *cms_get_theme_banner(const char *key, int epoch);
char *cms_get_theme_footer(const char *key, int epoch);

// The *stored* value only ("" if unset - not file-resolved), one per
// epoch (index via epoch_to_index()), for the admin form: it needs to
// tell "nothing saved" apart from "saved text that happens to equal the
// file". out_values is caller-allocated with EPOCH_COUNT entries, each
// filled with a malloc'd string the caller must free.
void cms_get_theme_banner_values(const char *key, char *out_values[EPOCH_COUNT]);
void cms_get_theme_footer_values(const char *key, char *out_values[EPOCH_COUNT]);

// db.themes.updateOne({key}, {$set: {"banner_html.<field for epoch>": html}},
// {upsert: true}). Returns 0 on success, -1 if epoch is outside -1..3, on
// a DB error, or if mongodb is not ready. An empty `html` clears that
// epoch back to the theme's on-disk default.
int cms_update_theme_banner(const char *key, int epoch, const char *html);
int cms_update_theme_footer(const char *key, int epoch, const char *html);

#endif // CMS_THEMES_H
