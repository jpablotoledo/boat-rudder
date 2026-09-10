#ifndef REQUEST_THEME_H
#define REQUEST_THEME_H

// The active theme key ("dark", "light", ...) for the request being served
// on this thread - a cross-cutting property the same way request_lang.h's
// language is: generate_url_theme() (several layers below the router, in
// nearly every module) needs it on every call. Rather than thread it
// through every signature, it is resolved once per request and stored per
// thread (one detached thread per connection, so a thread never serves two
// requests at once - same reasoning as request_lang.h).
//
// Precedence mirrors request_lang.h exactly, so switching themes works the
// same way switching languages does - a visitor's own choice, not a
// site-wide setting they have no control over:
//   1. `theme_query` (the raw `?theme=` value, if any) - wins when it
//      names a real html/themes/<key>/ directory. Exists for the same
//      reason lang_query does: epoch 0/1 have no reliable cookie-setting
//      redirect, so their theme-switch links go straight to "<page>?theme=x".
//   2. the `theme` cookie, if it names a real directory.
//   3. site_settings.active_theme (the site's own default for visitors who
//      have never chosen), falling back to configs/settings.conf's `theme`
//      - both via cms_get_active_theme_key().

// Resolves the active theme from `cookie_header`/`theme_query` (either may
// be NULL) per the precedence above, and stores it for request_theme().
// Call once per request, alongside request_lang_set()/request_user_set()
// in http_router.c.
void request_theme_set(const char *cookie_header, const char *theme_query);

// The theme key resolved by the last request_theme_set() on this thread.
// Never NULL/empty. The pointer stays valid until the next
// request_theme_set() on the same thread.
const char *request_theme(void);

#endif // REQUEST_THEME_H
