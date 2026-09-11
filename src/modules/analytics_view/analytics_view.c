#include "analytics_view.h"
#include "../../db/mongodb_manager.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/http_utils.h"
#include "../../utils/read_file.h"
#include "../../utils/template_utils.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VISITS_DAILY_COLLECTION       "page_visits_daily"
#define ENTRY_VISITS_DAILY_COLLECTION "entry_visits_daily"

#define MAX_KV 256

// One tallied key ("Chrome/124", "Windows/10", "US", ...) and its running
// count, for the browser/OS/country/route breakdowns - each day-bucket
// document in the queried range gets merged into one of these arrays.
typedef struct {
    char key[128];
    int count;
} KV;

typedef struct {
    int total, epochwml, epoch0, epoch1, epoch2, epoch3;
    KV browsers[MAX_KV];  int browser_count;
    KV oses[MAX_KV];      int os_count;
    KV countries[MAX_KV]; int country_count;
    KV routes[32];        int route_count;
} AnalyticsData;

static void kv_increment(KV *arr, int *count, int max, const char *key, int delta) {
    for (int i = 0; i < *count; i++) {
        if (strcmp(arr[i].key, key) == 0) { arr[i].count += delta; return; }
    }
    if (*count < max) {
        snprintf(arr[*count].key, sizeof(arr[*count].key), "%s", key);
        arr[*count].count = delta;
        (*count)++;
    }
}

static void kv_sort(KV *arr, int count) {
    for (int i = 0; i < count - 1; i++) {
        int max_i = i;
        for (int j = i + 1; j < count; j++)
            if (arr[j].count > arr[max_i].count) max_i = j;
        if (max_i != i) {
            KV tmp = arr[i];
            arr[i] = arr[max_i];
            arr[max_i] = tmp;
        }
    }
}

// Merges every field of one page_visits_daily document into `d` - `total`,
// `by_epoch`'s five named counters, and the three open-ended by_browser/
// by_os/by_country subdocuments (walked generically, since their field
// names are the dynamic keys analytics.c wrote) plus by_route's five.
static void merge_daily_doc(const bson_t *doc, AnalyticsData *d) {
    bson_iter_t iter, sub;

    if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
        d->total += bson_iter_int32(&iter);

    if (bson_iter_init_find(&iter, doc, "by_epoch") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub)) {
            if (!BSON_ITER_HOLDS_INT32(&sub)) continue;
            const char *key = bson_iter_key(&sub);
            int v = bson_iter_int32(&sub);
            if (strcmp(key, "epochwml") == 0) d->epochwml += v;
            else if (strcmp(key, "epoch0") == 0) d->epoch0 += v;
            else if (strcmp(key, "epoch1") == 0) d->epoch1 += v;
            else if (strcmp(key, "epoch2") == 0) d->epoch2 += v;
            else if (strcmp(key, "epoch3") == 0) d->epoch3 += v;
        }
    }

    if (bson_iter_init_find(&iter, doc, "by_browser") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(d->browsers, &d->browser_count, MAX_KV, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_os") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(d->oses, &d->os_count, MAX_KV, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_country") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(d->countries, &d->country_count, MAX_KV, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
    if (bson_iter_init_find(&iter, doc, "by_route") && bson_iter_recurse(&iter, &sub)) {
        while (bson_iter_next(&sub))
            if (BSON_ITER_HOLDS_INT32(&sub))
                kv_increment(d->routes, &d->route_count, 32, bson_iter_key(&sub), bson_iter_int32(&sub));
    }
}

// Builds the {filter} shared by query_page_visits()/query_top_articles():
// day -> exact key match; week/month -> {year, <week|month>}; year -> just
// {year}; range -> a "gte/lte" bound; all -> {} (everything). `id_field` is
// "_id" for page_visits_daily (whose _id IS the "YYYY-MM-DD" key, so range
// queries can compare it directly - lexicographic order matches
// chronological order for that format) and "date" for entry_visits_daily
// (whose _id is "<type>__<slug>__<date>", not orderable the same way).
static bson_t *build_period_filter(const char *id_field, const char *period,
                                    int year, int month, int week,
                                    const char *date, const char *from, const char *to) {
    if (strcmp(period, "day") == 0)
        return BCON_NEW(id_field, BCON_UTF8(date));
    if (strcmp(period, "week") == 0)
        return BCON_NEW("year", BCON_INT32(year), "week", BCON_INT32(week));
    if (strcmp(period, "year") == 0)
        return BCON_NEW("year", BCON_INT32(year));
    if (strcmp(period, "all") == 0)
        return bson_new();
    if (strcmp(period, "range") == 0)
        return BCON_NEW(id_field, "{", "$gte", BCON_UTF8(from), "$lte", BCON_UTF8(to), "}");
    // "month" - also the fallback for any unrecognized value.
    return BCON_NEW("year", BCON_INT32(year), "month", BCON_INT32(month));
}

static void query_page_visits(AnalyticsData *d, const char *period, int year, int month,
                               int week, const char *date, const char *from, const char *to) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(VISITS_DAILY_COLLECTION);
    if (!collection) return;

    bson_t *filter = build_period_filter("_id", period, year, month, week, date, from, to);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, NULL, NULL);

    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) merge_daily_doc(doc, d);

    bson_destroy(filter);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
}

// Top 20 articles/pages/categories by total visits in the period - a
// separate query (not derivable from page_visits_daily) since that
// collection has no per-slug breakdown at all.
static char *build_top_articles_table(const char *period, int year, int month, int week,
                                       const char *date, const char *from, const char *to) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(ENTRY_VISITS_DAILY_COLLECTION);
    if (!collection) return strdup("<p>No data</p>");

    bson_t *filter = build_period_filter("date", period, year, month, week, date, from, to);
    bson_t *opts = BCON_NEW(
        "sort", "{", "total", BCON_INT32(-1), "}",
        "limit", BCON_INT64((int64_t)20)
    );

    // Merge per-slug across the queried range the same way page_visits_daily
    // gets merged - a slug can have one bucket document per day.
    KV articles[64];
    int article_count = 0;

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, filter, NULL, NULL);
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        char label[160] = "";
        const char *entry_type = "", *slug = "";
        if (bson_iter_init_find(&iter, doc, "entry_type") && BSON_ITER_HOLDS_UTF8(&iter))
            entry_type = bson_iter_utf8(&iter, NULL);
        if (bson_iter_init_find(&iter, doc, "slug") && BSON_ITER_HOLDS_UTF8(&iter))
            slug = bson_iter_utf8(&iter, NULL);
        snprintf(label, sizeof(label), "[%s] %s", entry_type, slug);

        int total = 0;
        if (bson_iter_init_find(&iter, doc, "total") && BSON_ITER_HOLDS_INT32(&iter))
            total = bson_iter_int32(&iter);

        kv_increment(articles, &article_count, 64, label, total);
    }
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    bson_destroy(filter);
    bson_destroy(opts);

    kv_sort(articles, article_count);
    if (article_count > 20) article_count = 20;

    if (article_count == 0) return strdup("<p>No data</p>");

    char *rows = strdup("");
    for (int i = 0; rows && i < article_count; i++) {
        char *label_enc = malloc(strlen(articles[i].key) * 6 + 1);
        if (!label_enc) { free(rows); rows = NULL; break; }
        html_encode(label_enc, articles[i].key, strlen(articles[i].key) * 6 + 1);

        char *row = render_template(
            "<tr><td>%s</td><td>%d</td></tr>", label_enc, articles[i].count);
        free(label_enc);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) return strdup("<p>No data</p>");

    char *table = render_template(
        "<table class=\"boat-rudder__dashboard__table\">"
        "<thead><tr><th>Article</th><th>Visits</th></tr></thead><tbody>%s</tbody></table>",
        rows);
    free(rows);
    return table ? table : strdup("<p>No data</p>");
}

// One breakdown table (Browsers/OSes/Countries/Routes): a name column and a
// bar built from a CSS width percentage next to the count, widest entry
// first (`arr` is already sorted by kv_sort() before this is called).
static char *build_kv_table(const KV *arr, int count, int total) {
    if (count == 0) return strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");

    int max_count = arr[0].count > 0 ? arr[0].count : 1;

    char *rows = strdup("");
    for (int i = 0; rows && i < count; i++) {
        int bar_w = max_count > 0 ? arr[i].count * 100 / max_count : 0;
        int pct = total > 0 ? arr[i].count * 100 / total : 0;

        char *key_enc = malloc(strlen(arr[i].key) * 6 + 1);
        if (!key_enc) { free(rows); rows = NULL; break; }
        html_encode(key_enc, arr[i].key, strlen(arr[i].key) * 6 + 1);

        char *row = render_template(
            "<tr><td>%s<span class=\"boat-rudder__analytics__bar\" style=\"width:%dpx\"></span></td>"
            "<td>%d <span class=\"boat-rudder__analytics__pct\">(%d%%)</span></td></tr>",
            key_enc, bar_w, arr[i].count, pct);
        free(key_enc);
        rows = row ? str_append(rows, row) : NULL;
        free(row);
    }
    if (!rows) return strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");

    char *table = render_template("<table class=\"boat-rudder__analytics__kv-table\">%s</table>", rows);
    free(rows);
    return table ? table : strdup("<p class=\"boat-rudder__dashboard__empty\">No data</p>");
}

static const char *MONTH_NAMES[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void make_period_label(char *out, size_t out_size, const char *period,
                               int year, int month, int week,
                               const char *date, const char *from, const char *to) {
    if (strcmp(period, "day") == 0) {
        snprintf(out, out_size, "%s", date);
    } else if (strcmp(period, "week") == 0) {
        snprintf(out, out_size, "Week %d / %d", week, year);
    } else if (strcmp(period, "year") == 0) {
        snprintf(out, out_size, "%d", year);
    } else if (strcmp(period, "all") == 0) {
        snprintf(out, out_size, "All Time");
    } else if (strcmp(period, "range") == 0) {
        snprintf(out, out_size, "%s \xe2\x80\x94 %s", from, to);
    } else {
        const char *name = (month >= 1 && month <= 12) ? MONTH_NAMES[month - 1] : "?";
        snprintf(out, out_size, "%s %d", name, year);
    }
}

char *analytics_view(int epoch, const char *period, int year, int month, int week,
                      const char *date, const char *from, const char *to) {
    if (!period || !period[0]) period = "month";

    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    int cur_year = t.tm_year + 1900;
    int cur_month = t.tm_mon + 1;
    char cur_week_buf[4] = {0};
    strftime(cur_week_buf, sizeof(cur_week_buf), "%V", &t);
    int cur_week = atoi(cur_week_buf);
    int prev_year = cur_year - 1;

    char today_str[40];
    snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d", cur_year, cur_month, t.tm_mday);

    if (!date || !date[0]) date = today_str;
    if (year <= 0) year = cur_year;
    if (month <= 0) month = cur_month;
    if (week <= 0) week = cur_week;
    if (!from || !from[0]) from = today_str;
    if (!to || !to[0]) to = today_str;

    AnalyticsData d = {0};
    query_page_visits(&d, period, year, month, week, date, from, to);

    kv_sort(d.browsers, d.browser_count);
    kv_sort(d.oses, d.os_count);
    kv_sort(d.countries, d.country_count);
    kv_sort(d.routes, d.route_count);

    char period_label[64];
    make_period_label(period_label, sizeof(period_label), period, year, month, week, date, from, to);

    #define ACTIVE_CLASS "boat-rudder__analytics__period-btn--active"
    const char *act_day      = strcmp(period, "day") == 0 ? ACTIVE_CLASS : "";
    const char *act_week     = strcmp(period, "week") == 0 ? ACTIVE_CLASS : "";
    const char *act_month    = strcmp(period, "month") == 0 ? ACTIVE_CLASS : "";
    const char *act_year     = (strcmp(period, "year") == 0 && year == cur_year) ? ACTIVE_CLASS : "";
    const char *act_lastyear = (strcmp(period, "year") == 0 && year == prev_year) ? ACTIVE_CLASS : "";
    const char *act_all      = strcmp(period, "all") == 0 ? ACTIVE_CLASS : "";
    const char *act_range    = strcmp(period, "range") == 0 ? ACTIVE_CLASS : "";

    char *tpl_path = generate_url_theme("dashboard/analytics/analytics_epoch%d.html", epoch);
    char *tpl = tpl_path ? read_file_to_string(tpl_path) : NULL;
    free(tpl_path);
    if (!tpl) return NULL;

    char *tbl_routes    = build_kv_table(d.routes, d.route_count, d.total);
    char *tbl_browsers  = build_kv_table(d.browsers, d.browser_count, d.total);
    char *tbl_oses      = build_kv_table(d.oses, d.os_count, d.total);
    char *tbl_countries = build_kv_table(d.countries, d.country_count, d.total);
    char *tbl_articles  = build_top_articles_table(period, year, month, week, date, from, to);

    char *result = NULL;
    if (tbl_routes && tbl_browsers && tbl_oses && tbl_countries && tbl_articles) {
        result = render_template(tpl,
            today_str, act_day,
            year, week, act_week,
            year, month, act_month,
            cur_year, act_year,
            prev_year, act_lastyear,
            act_all,
            act_range, from, to,
            period_label,
            d.total, d.epochwml, d.epoch0, d.epoch1, d.epoch2, d.epoch3,
            tbl_routes, tbl_browsers, tbl_oses, tbl_countries, tbl_articles);
    }

    free(tpl);
    free(tbl_routes);
    free(tbl_browsers);
    free(tbl_oses);
    free(tbl_countries);
    free(tbl_articles);
    return result;
}
