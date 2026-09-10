#ifndef THEME_PAGE_H
#define THEME_PAGE_H

// Builds the /theme page: every theme discovered under html/themes/ as a
// plain link, working the same way /language does (language_page.h) -
// epoch 3's nav bar offers the same list as a drop-down
// (menu.c's theme_selector()), but epoch 1/2 link here instead, since a
// drop-down cannot be relied on there. Epoch 1 links directly to
// "<return_url>?theme=<key>" (a one-click, one-request choice - no cookie
// set, same Mosaic-era reasoning language_page.c documents for its own
// epoch 1 case); epoch 2 links to /theme/set, which sets the `theme`
// cookie and bounces back.
//
// `return_url` is the page to return to; it is validated by the caller
// (language_sanitize_return(), reused as-is - see language_page.h) and
// falls back to "/" when NULL or empty.
//
// Returns a malloc'd string, or NULL if a template is missing (only
// epoch 1/2 templates exist - this page is not offered on other epochs
// today) or on allocation failure. The caller must free() it.
char *theme_page(int epoch, const char *return_url);

#endif // THEME_PAGE_H
