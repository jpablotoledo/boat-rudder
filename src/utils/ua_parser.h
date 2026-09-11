#ifndef UA_PARSER_H
#define UA_PARSER_H

#include <stddef.h>

// Lightweight User-Agent -> browser/OS classification for analytics.c's
// per-day aggregation. No regex, no bundled UA database - a straight,
// priority-ordered chain of substring checks (order matters: e.g. Edge and
// Opera both inject "Chrome/" and must be checked first). Output is always
// NUL-terminated and safe to use as a MongoDB field-name segment (see
// analytics.c's mongo_key_sanitize() - callers still route every result
// through that before using it in a "$inc" path, since a handful of branches
// here embed a literal '.' in the *name* itself, e.g. "UP.Browser").
void ua_parse_browser(const char *user_agent, char *out, size_t size);
void ua_parse_os(const char *user_agent, char *out, size_t size);

#endif // UA_PARSER_H
