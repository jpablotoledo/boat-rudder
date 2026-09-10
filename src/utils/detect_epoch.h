#ifndef DETECT_EPOCH_H
#define DETECT_EPOCH_H

// Browser "epochs" used to select retro-compatible HTML templates.
#define EPOCH_WML         (-1)  // WAP 1.x devices (WML/WAP browsers)
#define EPOCH_PRESTANDARD   0   // Text-only browsers (Lynx, w3m, ...)
#define EPOCH_EARLY         1   // Mosaic, Netscape 1-3, IE <=4 (no CSS/JS)
#define EPOCH_MIDDLE        2   // Netscape 4, IE 5-10, Firefox <4, Chrome <10
#define EPOCH_MODERN        3   // Modern browsers (HTML5 + CSS3)

// Classifies a User-Agent header value into a browser epoch.
// user_agent may be NULL or empty, in which case EPOCH_EARLY is returned.
int detect_epoch(const char *user_agent);

// Number of epochs (-1..3 inclusive) - the size of any array indexed by
// epoch_to_index(), e.g. a theme's per-epoch banner/footer values.
#define EPOCH_COUNT 5

// Maps epoch (-1..3) to a 0..4 array index, or -1 if `epoch` is out of
// that range. Was cms_site_settings_epoch_index() (site-personalization-
// plan.md) before banner/footer moved to being theme-scoped - moved here
// because it maps epochs generically, not anything specific to site
// settings or themes.
int epoch_to_index(int epoch);

#endif // DETECT_EPOCH_H
