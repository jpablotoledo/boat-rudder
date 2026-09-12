// Records one row of aggregated visit data per calendar day (the "bucket"
// pattern - one document per day, incremented in place, rather than one row
// per visit): db.page_visits_daily holds the site-wide totals, keyed by
// "YYYY-MM-DD"; db.entry_visits_daily holds the same breakdown per blog
// post/page/category, keyed by "<entry_type>__<slug>__YYYY-MM-DD". Both are
// upserted with a single $inc per write (see upsert_daily_bucket()), so
// there is no raw per-visit event log and nothing to prune - a day's
// document is exactly as large on day 1000 of traffic as on day 1.
//
// page_visits_daily shape:
//   { _id, date, year, month, week, total,
//     by_epoch: {epochwml,epoch0,epoch1,epoch2,epoch3},
//     by_browser: {<key>: n, ...}, by_os: {<key>: n, ...},
//     by_country: {<key>: n, ...}, by_route: {home,blog,page,gallery,login,other} }
// entry_visits_daily shape (only written when the URL resolves to a
// specific slug):
//   { _id, date, year, month, week, entry_type, slug, total,
//     by_epoch: {epochwml,epoch0,epoch1,epoch2,epoch3} }
//
// Ported from the-retro-center-old's analytics.c, adapted to boat-rudder's
// route shapes and mongodb_manager.c.
#include "analytics.h"
#include "geoip.h"
#include "../../utils/log.h"
#include "../../utils/ua_parser.h"
#include "../../db/mongodb_manager.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define VISITS_DAILY_COLLECTION       "page_visits_daily"
#define ENTRY_VISITS_DAILY_COLLECTION "entry_visits_daily"

// Substrings of a User-Agent that mark it as a crawler/bot rather than a
// real visitor - hand-rolled case-insensitive scan since strcasestr() isn't
// standard C.
static const char *BOT_TOKENS[] = {
    "bot", "crawl", "spider", "slurp", "mediapartners",
    "facebookexternalhit", "baiduspider", "yandex", "sogou",
    "ia_archiver", "archive.org", "pingdom", "uptimerobot",
    "curl/", "python-requests", "wget/", "java/", "go-http", NULL
};

// URL prefixes never worth tracking - the admin area itself (which would
// otherwise pollute the very report it's building).
static const char *SKIP_PREFIXES[] = { "/dashboard", NULL };

static const char *STATIC_EXTENSIONS[] = {
    ".css", ".js", ".png", ".jpg", ".jpeg", ".gif", ".ico",
    ".svg", ".woff", ".woff2", ".ttf", ".otf", ".eot",
    ".webp", ".mp4", ".webm", ".pdf", ".zip", ".gz",
    ".map", ".json", ".wbmp", NULL
};

static int is_bot(const char *ua) {
    if (!ua) return 0;
    size_t ua_len = strlen(ua);
    char lower[512];
    size_t n = ua_len < sizeof(lower) - 1 ? ua_len : sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)ua[i]);
    lower[n] = '\0';

    for (int i = 0; BOT_TOKENS[i]; i++) {
        if (strstr(lower, BOT_TOKENS[i])) return 1;
    }
    return 0;
}

static int has_static_extension(const char *url) {
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);

    const char *dot = NULL;
    for (size_t i = len; i > 0; i--) {
        if (url[i - 1] == '.') { dot = url + i - 1; break; }
        if (url[i - 1] == '/') break;
    }
    if (!dot) return 0;

    char ext[16];
    size_t ext_len = (size_t)(url + len - dot);
    if (ext_len >= sizeof(ext)) return 0;
    for (size_t i = 0; i < ext_len; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[ext_len] = '\0';

    for (int i = 0; STATIC_EXTENSIONS[i]; i++) {
        if (strcmp(ext, STATIC_EXTENSIONS[i]) == 0) return 1;
    }
    return 0;
}

static int should_skip_url(const char *url) {
    for (int i = 0; SKIP_PREFIXES[i]; i++) {
        size_t plen = strlen(SKIP_PREFIXES[i]);
        if (strncmp(url, SKIP_PREFIXES[i], plen) == 0) return 1;
    }
    return has_static_extension(url);
}

// Splits `url` into a route group (one of by_route's fixed keys) plus,
// where the route names one specific piece of content, an entry_type/slug
// pair for entry_visits_daily - mirrors boat-rudder's actual route table
// in http_router.c (see "/blog", "/blog/category/<slug>", "/blog/<slug>",
// "/page/<slug>", "/gallery/<id>"), not the sibling project's own routes.
static void classify_url(const char *url, char *group, size_t group_size,
                          char *slug, size_t slug_size,
                          char *entry_type, size_t entry_type_size) {
    group[0] = '\0';
    slug[0] = '\0';
    entry_type[0] = '\0';

    if (url[0] == '\0' || strcmp(url, "/") == 0) {
        snprintf(group, group_size, "home");
    } else if (strcmp(url, "/login") == 0) {
        snprintf(group, group_size, "login");
    } else if (strncmp(url, "/blog/category/", 15) == 0 && url[15] != '\0') {
        snprintf(group, group_size, "blog");
        snprintf(entry_type, entry_type_size, "category");
        snprintf(slug, slug_size, "%s", url + 15);
    } else if (strncmp(url, "/blog/", 6) == 0 && url[6] != '\0') {
        snprintf(group, group_size, "blog");
        snprintf(entry_type, entry_type_size, "blog");
        snprintf(slug, slug_size, "%s", url + 6);
    } else if (strcmp(url, "/blog") == 0) {
        snprintf(group, group_size, "blog");
    } else if (strncmp(url, "/page/", 6) == 0 && url[6] != '\0') {
        snprintf(group, group_size, "page");
        snprintf(entry_type, entry_type_size, "page");
        snprintf(slug, slug_size, "%s", url + 6);
    } else if (strncmp(url, "/gallery/", 9) == 0 && url[9] != '\0') {
        snprintf(group, group_size, "gallery");
    } else {
        snprintf(group, group_size, "other");
    }
}

// A dynamic key ends up as a dotted-path segment in a MongoDB "$inc"
// ("by_os.Windows/8_1"), so it can't itself contain '.' or '$' - most
// ua_parser.c branches already avoid that, but this is the one place that
// defends every key regardless of source (also covers a stray '.'/'$' in a
// raw, unrecognized User-Agent's fallback key).
static void mongo_key_sanitize(char *key) {
    for (char *p = key; *p; p++) {
        if (*p == '.' || *p == '$') *p = '_';
    }
}

static void upsert_daily_bucket(const char *collection_name, const char *doc_id,
                                 const bson_t *set_on_insert, const bson_t *inc) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(collection_name);
    if (!collection) return;

    bson_t *filter = BCON_NEW("_id", BCON_UTF8(doc_id));
    bson_t *update = bson_new();
    bson_append_document(update, "$setOnInsert", -1, set_on_insert);
    bson_append_document(update, "$inc", -1, inc);
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, filter, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("analytics: upsert into %s failed: %s", collection_name, error.message);

    bson_destroy(filter);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
}

static const char *epoch_bucket_name(int epoch) {
    switch (epoch) {
        case -1: return "epochwml";
        case 0:  return "epoch0";
        case 1:  return "epoch1";
        case 2:  return "epoch2";
        default: return "epoch3";
    }
}

void analytics_track_visit(const char *method, const char *url,
                            const char *user_agent, const char *client_ip, int epoch) {
    if (!method || strcmp(method, "GET") != 0) return;
    if (!url || !url[0]) return;
    if (should_skip_url(url)) return;
    if (is_bot(user_agent)) return;

    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);

    int year = t.tm_year + 1900;
    int month = t.tm_mon + 1;
    char week_buf[4] = {0};
    strftime(week_buf, sizeof(week_buf), "%V", &t);
    int week = atoi(week_buf);

    char day_key[40];
    snprintf(day_key, sizeof(day_key), "%04d-%02d-%02d", year, month, t.tm_mday);

    char browser[96], os[96];
    ua_parse_browser(user_agent, browser, sizeof(browser));
    ua_parse_os(user_agent, os, sizeof(os));
    mongo_key_sanitize(browser);
    mongo_key_sanitize(os);

    char country[64];
    geoip_lookup(client_ip, country, sizeof(country));
    mongo_key_sanitize(country);

    char group[16], slug[256], entry_type[16];
    classify_url(url, group, sizeof(group), slug, sizeof(slug), entry_type, sizeof(entry_type));

    bson_t set_on_insert;
    bson_init(&set_on_insert);
    bson_append_utf8(&set_on_insert, "date", -1, day_key, -1);
    bson_append_int32(&set_on_insert, "year", -1, year);
    bson_append_int32(&set_on_insert, "month", -1, month);
    bson_append_int32(&set_on_insert, "week", -1, week);

    bson_t inc;
    bson_init(&inc);
    bson_append_int32(&inc, "total", -1, 1);
    char path[128];
    snprintf(path, sizeof(path), "by_epoch.%s", epoch_bucket_name(epoch));
    bson_append_int32(&inc, path, -1, 1);
    snprintf(path, sizeof(path), "by_browser.%s", browser);
    bson_append_int32(&inc, path, -1, 1);
    snprintf(path, sizeof(path), "by_os.%s", os);
    bson_append_int32(&inc, path, -1, 1);
    snprintf(path, sizeof(path), "by_country.%s", country);
    bson_append_int32(&inc, path, -1, 1);
    snprintf(path, sizeof(path), "by_route.%s", group);
    bson_append_int32(&inc, path, -1, 1);

    upsert_daily_bucket(VISITS_DAILY_COLLECTION, day_key, &set_on_insert, &inc);
    bson_destroy(&set_on_insert);
    bson_destroy(&inc);

    if (slug[0] && entry_type[0]) {
        char entry_id[600];
        snprintf(entry_id, sizeof(entry_id), "%s__%s__%s", entry_type, slug, day_key);

        bson_t entry_soi;
        bson_init(&entry_soi);
        bson_append_utf8(&entry_soi, "date", -1, day_key, -1);
        bson_append_int32(&entry_soi, "year", -1, year);
        bson_append_int32(&entry_soi, "month", -1, month);
        bson_append_int32(&entry_soi, "week", -1, week);
        bson_append_utf8(&entry_soi, "entry_type", -1, entry_type, -1);
        bson_append_utf8(&entry_soi, "slug", -1, slug, -1);

        bson_t entry_inc;
        bson_init(&entry_inc);
        bson_append_int32(&entry_inc, "total", -1, 1);
        snprintf(path, sizeof(path), "by_epoch.%s", epoch_bucket_name(epoch));
        bson_append_int32(&entry_inc, path, -1, 1);

        upsert_daily_bucket(ENTRY_VISITS_DAILY_COLLECTION, entry_id, &entry_soi, &entry_inc);
        bson_destroy(&entry_soi);
        bson_destroy(&entry_inc);
    }

    LOG_DEBUG("analytics: tracked %s (browser=%s os=%s route=%s)", url, browser, os, group);
}
