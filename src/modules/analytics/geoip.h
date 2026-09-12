#ifndef GEOIP_H
#define GEOIP_H

#include <stddef.h>

// Country detection for analytics.c's by_country breakdown, via MaxMind's
// GeoLite2-Country database (libmaxminddb). Fully optional: when the
// project isn't built with libmaxminddb available (see CMakeLists.txt's
// HAVE_MAXMINDDB gate), every function here is a no-op/stub and
// geoip_lookup() always writes "Unknown" - the feature degrades gracefully
// rather than failing to build.

// Opens `mmdb_path` (falls back to GEOIP_DEFAULT_DB_PATH if NULL/empty).
// Returns 0 on success, -1 on failure (missing file, bad format, ...) -
// either way the caller can keep running, just with geoip_lookup() always
// returning "Unknown". Call once at startup, after mongodb_manager_init().
int geoip_init(const char *mmdb_path);

// Closes the database. Safe to call even if geoip_init() was never called
// or failed. Call once at shutdown, before mongodb_manager_cleanup().
void geoip_cleanup(void);

// Resolves `ip` (IPv4 or IPv6 string) to a country name (e.g. "Chile"),
// falling back to its ISO 3166-1 alpha-2 code (e.g. "CL") if no English
// name is in the database, written into `out` (truncated to size - 1,
// always NUL-terminated). Writes "Unknown" for private/loopback ranges, a
// lookup miss, or when the database isn't open at all.
void geoip_lookup(const char *ip, char *out, size_t size);

// Default path for the GeoLite2-Country database (can be overridden at
// init) - place the file here to enable country detection.
#define GEOIP_DEFAULT_DB_PATH "./data/GeoLite2-Country.mmdb"

#endif // GEOIP_H
