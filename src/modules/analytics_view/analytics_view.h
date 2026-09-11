#ifndef ANALYTICS_VIEW_H
#define ANALYTICS_VIEW_H

// Builds the /dashboard/analytics report fragment: a period selector, total/
// per-epoch visit counts, browser/OS/country/route breakdowns, and a top-20
// articles table, all aggregated from analytics.c's page_visits_daily/
// entry_visits_daily day-buckets.
//
// `period` is one of "day"/"week"/"month"/"year"/"all"/"range" ("month" if
// NULL/empty/unrecognized); the other params narrow it: `date` for "day"
// ("YYYY-MM-DD", defaults to today UTC), `year`/`week` for "week",
// `year`/`month` for "month", `year` alone for "year", `from`/`to`
// ("YYYY-MM-DD" each) for "range". Ignored where not relevant to `period`.
//
// Epoch 3 only - the report itself is admin tooling, gated the same way as
// every other /dashboard/settings page (see http_router.c's route), so
// there is exactly one template (analytics_epoch3.html) instead of one per
// epoch. Returns a malloc'd string, or NULL on a missing template or
// allocation failure.
char *analytics_view(int epoch, const char *period, int year, int month, int week,
                      const char *date, const char *from, const char *to);

#endif // ANALYTICS_VIEW_H
