#ifndef THEME_CATALOG_H
#define THEME_CATALOG_H

#include <stddef.h>

// Discovers available themes by reading html/themes/ directly - no DB
// catalog/registration step (see theme-system-plan.md §5): drop a
// directory in, it shows up both in /dashboard/settings/themes and in the
// public navbar's theme switcher (menu.c's theme_selector()).
//
// *out is a malloc'd array of *out_count malloc'd key strings (caller
// frees each and the array via free_theme_keys()); an unreadable
// html/themes/ directory yields *out = NULL, *out_count = 0.
void list_theme_keys(char ***out, size_t *out_count);

// Frees an array returned by list_theme_keys(). Safe to call with keys ==
// NULL.
void free_theme_keys(char **keys, size_t count);

// 1 iff html/themes/<key>/ exists and is a directory, 0 otherwise
// (including a NULL/empty key). Used to validate a theme key coming from
// a cookie, query string, or admin form before trusting it.
int theme_key_is_valid(const char *key);

#endif // THEME_CATALOG_H
