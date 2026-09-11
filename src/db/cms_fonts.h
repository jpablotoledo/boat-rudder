#ifndef CMS_FONTS_H
#define CMS_FONTS_H

#include <stddef.h>

// Uploaded font files, global (not per-theme) - a theme only stores which
// one it picked for its epoch 3 navbar logo text (see cms_themes.h's
// cms_get_theme_logo_font()/cms_update_theme_logo_font()). Files live under
// html/assets/fonts/<filename>, next to the site's own built-in Milonga
// (which isn't a `fonts` document - it's wired directly into
// styles_epoch3.css and stays the default when a theme has picked nothing).
typedef struct {
    char *id;       // hex ObjectId (24 chars + NUL)
    char *name;     // display name, and the CSS font-family value themes reference
    char *filename; // under html/assets/fonts/, e.g. "MyFont-Regular.ttf"
} CmsFont;

// db.fonts.find().sort({name: 1}). On success, *out points to a malloc'd
// array of *out_count items (possibly 0) that must be passed to
// cms_fonts_free(). On a DB error or if mongodb is not ready, *out = NULL
// and *out_count = 0.
void cms_get_fonts(CmsFont **out, size_t *out_count);

// Frees every item's fields and the array itself. Safe to call with fonts == NULL.
void cms_fonts_free(CmsFont *fonts, size_t count);

// Inserts {name, filename}. Returns 0 on success, -1 on a DB error or if
// mongodb is not ready.
int cms_add_font(const char *name, const char *filename);

// db.fonts.findOne({_id: id_hex}).filename, for the delete route to know
// which file to unlink alongside the document. Writes into `out`
// (truncated to out_size - 1), "" if id_hex is invalid, not found, or
// mongodb is not ready. Returns 1 if found, 0 otherwise.
int cms_get_font_filename(const char *id_hex, char *out, size_t out_size);

// db.fonts.deleteOne({_id: id_hex}). Returns 0 on success, -1 if id_hex is
// invalid, on a DB error, or if mongodb is not ready.
int cms_delete_font(const char *id_hex);

// db.fonts.findOne({name}).filename, for the epoch 3 render path: a theme
// only stores the font *name* it picked (cms_get_theme_logo_font()), this
// resolves it to the actual file to declare an @font-face for. Returns a
// malloc'd string, "" if `name` is empty/unknown or mongodb is not ready
// (never NULL unless allocation fails).
char *cms_get_font_filename_by_name(const char *name);

// The extension a font was uploaded with, mapped to the format() hint an
// @font-face src needs - browsers won't reliably guess it from the URL
// alone. "" (empty format(), which every browser just ignores) for
// anything unrecognized rather than failing the whole declaration. Shared
// by page_layout.c's live @font-face injection and fonts_admin.c's preview
// of each uploaded font in its own name.
const char *cms_font_format_for_filename(const char *filename);

#endif // CMS_FONTS_H
