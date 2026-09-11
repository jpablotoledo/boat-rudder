#ifndef ANALYTICS_H
#define ANALYTICS_H

// Records one page visit into MongoDB's day-bucket collections
// (page_visits_daily / entry_visits_daily - see analytics.c's own doc
// comment for the exact schema), for the /dashboard/analytics report
// (analytics_view.h) to aggregate later. Call once per request, before
// routing, with the already-decoded, query-string-stripped URL - see
// http_router.c's call site.
//
// A no-op (nothing written) for: any method other than GET; a bot/crawler
// User-Agent; a request under /dashboard (the admin area itself, which
// would otherwise pollute the very report it's building); or a static
// asset (matched by file extension, not path - it still filters images/
// CSS/JS served from anywhere, e.g. /themes/<key>/assets/...).
void analytics_track_visit(const char *method, const char *url,
                            const char *user_agent, const char *client_ip, int epoch);

#endif // ANALYTICS_H
