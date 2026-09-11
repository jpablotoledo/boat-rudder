#ifndef FONTS_ADMIN_H
#define FONTS_ADMIN_H

// /dashboard/settings/fonts - the global font library (see cms_fonts.h):
// a table of uploaded fonts with a delete action each, plus an upload form
// (name + file). Every theme's "Logo font" picker
// (settings-themes-panel_epoch3.html) draws from this same list.
// `error_message` is shown above the table if non-NULL/non-empty. Returns a
// malloc'd string, or NULL on a missing template / allocation failure.
char *fonts_admin_list(int epoch, const char *error_message);

#endif // FONTS_ADMIN_H
