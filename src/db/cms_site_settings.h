#ifndef CMS_SITE_SETTINGS_H
#define CMS_SITE_SETTINGS_H

// site_settings is a singleton holding what's genuinely site-wide, not
// theme-specific: the site's name and which theme is currently active.
// Banner/footer content and colors are theme-scoped and live in `themes`
// instead (src/db/cms_themes.h) - see theme-scoped-personalization-plan.md
// for why banner/footer moved out of here.

// Returns a malloc'd copy of site_settings.site_name, or "Boat Rudder" if
// unset/unreachable. Never NULL - site personalization must never fail a
// page.
char *cms_get_site_name(void);

// db.site_settings.updateOne({}, {$set: {site_name}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_site_name(const char *name);

// Returns a malloc'd theme key: site_settings.active_theme if set and a
// directory exists at html/themes/<key>/, otherwise
// configs/settings.conf's `theme` value. Never NULL/empty - "which theme
// renders this request" must never fail. This is what request_theme.c
// calls once per request; render-path code should go through
// request_theme() instead of calling this directly.
char *cms_get_active_theme_key(void);

// db.site_settings.updateOne({}, {$set: {active_theme: key}}, {upsert:
// true}). Returns 0 on success, -1 if html/themes/<key>/ does not exist,
// on a DB error, or if mongodb is not ready.
int cms_set_active_theme(const char *key);

#endif // CMS_SITE_SETTINGS_H
