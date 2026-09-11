#define _XOPEN_SOURCE 700

#include "http_router.h"
#include "http_constants.h"
#include "connection.h"
#include "http_request_parser.h"
#include "utils/url_parser.h"
#include "utils/static_file_server.h"
#include "../db/auth.h"
#include "../db/cms_categories.h"
#include "../db/cms_entries.h"
#include "../db/cms_entries_admin.h"
#include "../db/cms_languages.h"
#include "../db/cms_menu.h"
#include "../db/cms_site_settings.h"
#include "../db/cms_fonts.h"
#include "../db/cms_themes.h"
#include "../db/cms_users_admin.h"
#include "../db/mongodb_manager.h"
#include "../db/session_manager.h"
#include "../html_builder/orchestrator.h"
#include "../modules/analytics/analytics.h"
#include "../modules/analytics_view/analytics_view.h"
#include "../modules/blog_list/blog_list.h"
#include "../modules/category_menu/category_menu.h"
#include "../modules/categories_admin/categories_admin.h"
#include "../modules/dashboard/dashboard.h"
#include "../modules/entry_editor/entry_editor.h"
#include "../modules/entry_editor/entry_editor_blocks.h"
#include "../modules/entry_page/entry_page.h"
#include "../modules/error/error.h"
#include "../modules/fonts_admin/fonts_admin.h"
#include "../modules/languages_admin/languages_admin.h"
#include "../modules/language_page/language_page.h"
#include "../modules/theme_page/theme_page.h"
#include "../modules/login/login.h"
#include "../modules/media_admin/media_admin.h"
#include "../modules/menu_admin/menu_admin.h"
#include "../modules/site_settings_admin/site_settings_admin.h"
#include "../modules/users_admin/users_admin.h"
#include "../db/cms_media.h"
#include "../db/cms_media_galleries.h"
#include "utils/multipart_parser.h"
#include "../utils/build_epoch_response.h"
#include "../utils/generate_url_theme.h"
#include "../utils/read_file.h"
#include "../utils/request_lang.h"
#include "../utils/request_theme.h"
#include "../utils/theme_catalog.h"
#include "../utils/request_user.h"
#include "../utils/config_loader.h"
#include "../utils/qr_generator/qr_generator.h"
#include "../utils/detect_epoch.h"
#include "../utils/json_utils.h"
#include "../utils/log.h"
#include "../utils/http_utils.h"
#include "../utils/template_utils.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *get_header_value(HttpRequest *req, const char *key) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].key, key) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

// HTTP/1.0 (RFC 1945) requires a redirect's Location to be an absolute URI;
// a bare path like "/" is only legal from RFC 2616 onward. Some HTTP/1.0-era
// clients (NCSA Mosaic among them) take that literally and instead of
// resolving the path against the current page, prepend "http://" straight
// onto it - "/" becomes the schemeless, hostless "http:///", which then
// fails as a DNS lookup on an empty hostname. Building the absolute form
// ourselves, from the Host the client itself just connected with, sidesteps
// the ambiguity entirely. Falls back to the bare path if the request had no
// Host header (rare, but then there is no better address to build from).
static char *absolute_location(void *ctx, HttpRequest *req, const char *path) {
    const char *host = get_header_value(req, "Host");
    if (!host || !host[0]) return NULL;

    const char *scheme = ((connection_ctx_t *)ctx)->ssl ? "https" : "http";
    size_t len = strlen(scheme) + strlen(host) + strlen(path) + 4;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s://%s%s", scheme, host, path);
    return out;
}

static void send_simple(void *ctx, const char *status, const char *body) {
    char header[512];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, body_len);
    connection_write(ctx, header, strlen(header));
    connection_write(ctx, body, body_len);
}

// Computes the browser "epoch" for `req`: the configured `force_epoch`
// override, if set; otherwise a "preview_epoch" query-string override, if
// present and valid; otherwise detect_epoch(User-Agent).
//
// preview_epoch backs /dashboard/settings/preview's iframe: every dynamic
// route needs to honor it (not just "/"), because the admin clicks through
// the real site inside that iframe - keepEpochOnNavigation() in
// preview_epoch3.html appends it to every same-origin link it finds after
// each navigation, so the chosen epoch survives clicking around instead of
// reverting to whatever User-Agent the browser actually sends. Reading it
// straight off req->url (rather than requiring every call site to have
// already parsed the query string into params[]) is what lets one change
// here cover every route.
static int resolve_epoch(HttpRequest *req) {
    if (force_epoch >= EPOCH_WML && force_epoch <= EPOCH_MODERN) return force_epoch;

    static const char preview_key[] = "preview_epoch=";
    const char *p = strstr(req->url, preview_key);
    if (p && (p == req->url || p[-1] == '?' || p[-1] == '&')) {
        int preview_epoch = atoi(p + sizeof(preview_key) - 1);
        if (preview_epoch >= EPOCH_WML && preview_epoch <= EPOCH_MODERN) return preview_epoch;
    }

    return detect_epoch(get_header_value(req, "User-Agent"));
}

// Renders the centralized error_epoch<N>.html template via error_content() +
// buildPageWebSite(), and sends it with `status_line`. Falls back to
// send_simple() if the template could not be loaded/rendered.
static void send_error_response(void *ctx, int status_code, const char *status_line, int epoch) {
    char title[64];
    snprintf(title, sizeof(title), "Boat Rudder - Error %d", status_code);

    char *content  = error_content(epoch, status_code, NULL);
    char *body     = buildPageWebSite(epoch, title, content);
    char *response = body ? build_epoch_response_status(body, "", epoch, status_line) : NULL;
    free(body);

    if (!response) {
        // Last-resort fallback: template loading itself failed, so this must
        // not depend on the filesystem/template pipeline. Plain text only.
        send_simple(ctx, status_line, status_line);
        return;
    }

    connection_write(ctx, response, strlen(response));
    free(response);
}

// Sends a malloc'd full HTTP response, truncating to headers-only for HEAD
// requests, and frees it. If `response` is NULL (some step failed), sends an
// epoch-aware 500 error page instead.
static void send_or_error(void *ctx, char *response, const char *method, int epoch) {
    if (!response) {
        send_error_response(ctx, 500, "500 Internal Server Error", epoch);
        return;
    }

    size_t response_len = strlen(response);
    if (strcmp(method, "HEAD") == 0) {
        const char *header_end = strstr(response, "\r\n\r\n");
        response_len = header_end ? (size_t)(header_end - response) + 4 : response_len;
    }
    connection_write(ctx, response, response_len);
    free(response);
}

// Looks up db.entries.findOne({link, enabled: true}) and renders it via
// entry_page() + buildPageWebSite(), used by both /page/<link>
// (expected_type "page") and /blog/<link> (expected_type "blog"). Sends a
// 404 if the entry doesn't exist, mongodb is not ready, or entry.type !=
// expected_type. Takes ownership of `category_menu_html` (pass NULL for
// non-blog pages).
static void serve_cms_entry(void *ctx, const char *link, const char *expected_type,
                             const char *lang, const char *method, int epoch,
                             char *category_menu_html, int page) {
    CmsEntry entry;
    if (mongodb_manager_is_ready() && cms_get_entry_by_link(link, lang, &entry)) {
        if (strcmp(entry.type, expected_type) != 0) {
            cms_entry_free(&entry);
            free(category_menu_html);
            send_error_response(ctx, 404, "404 Not Found", epoch);
            return;
        }

        // For blog entries highlight the "/blog" menu item (section match).
        // For pages use the full "/page/<link>" URL (menu items point to specific pages).
        char current_url[256];
        if (strcmp(expected_type, "blog") == 0)
            snprintf(current_url, sizeof(current_url), "/blog");
        else
            snprintf(current_url, sizeof(current_url), "/page/%s", link);

        char *title   = strdup(entry.header_title);
        char *content = entry_page(&entry, epoch, page, NULL);
        cms_entry_free(&entry);

        char *body = NULL;
        if (content && title) {
            body = buildEntryWebSiteAtUrl(epoch, title, content, current_url, category_menu_html);
            category_menu_html = NULL; // ownership transferred
        } else {
            free(content);
            free(category_menu_html);
        }
        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
        free(title);
        free(body);
        send_or_error(ctx, response, method, epoch);
    } else {
        free(category_menu_html);
        send_error_response(ctx, 404, "404 Not Found", epoch);
    }
}

// Shared by every /dashboard/* sub-route: sends a 503 and returns 0 if
// mongodb is not ready; sends a 302 to /login and returns 0 if `req` has no
// valid session cookie; otherwise writes the session's user id into
// user_id_out (>= USER_ID_HEX_BUF_SIZE bytes) and returns 1. Callers must not
// write to `ctx` if this returns 0.
static int require_dashboard_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    if (!mongodb_manager_is_ready()) {
        send_error_response(ctx, 503, "503 Service Unavailable", epoch);
        return 0;
    }

    const char *cookie = get_header_value(req, "Cookie");
    if (validate_session_cookie(cookie, user_id_out) != 1) {
        char *response = build_redirect_response("/login", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    return 1;
}

// require_dashboard_session() + cms_get_user_role(): role_out (>= USER_ROLE_BUF_SIZE
// bytes) is "admin" or "author" (a missing `role` field, or a lookup error, defaults
// to "admin" - same backward-compatible convention as cms_get_user_role()). Same
// 0/1 return + ctx-write contract as require_dashboard_session().
static int require_dashboard_session_role(void *ctx, HttpRequest *req, int epoch,
                                            char *user_id_out, char *role_out, size_t role_size) {
    if (!require_dashboard_session(ctx, req, epoch, user_id_out)) return 0;

    if (cms_get_user_role(user_id_out, role_out, role_size) != 0) {
        strncpy(role_out, "admin", role_size - 1);
        role_out[role_size - 1] = '\0';
    }

    return 1;
}

// require_dashboard_session_role() + role != "admin" -> 302 /dashboard, return 0.
// Used by routes reserved to Administrador (Users maintainer, entry deletion).
static int require_admin_session(void *ctx, HttpRequest *req, int epoch, char *user_id_out) {
    char role[USER_ROLE_BUF_SIZE];
    if (!require_dashboard_session_role(ctx, req, epoch, user_id_out, role, sizeof(role))) return 0;

    if (strcmp(role, "admin") != 0) {
        char *response = build_redirect_response("/dashboard", "", epoch);
        send_or_error(ctx, response, req->method, epoch);
        return 0;
    }

    return 1;
}

// 1 if `role` is "admin", or `role` is "author" and `entry` is a "blog" entry
// created by `user_id`. Gates the entry editor GET/POST routes - an Autor may
// only edit their own blog entries.
static int can_edit_entry(const char *role, const char *user_id, const CmsEntryEdit *entry) {
    if (strcmp(role, "admin") == 0) return 1;

    return strcmp(role, "author") == 0 &&
           strcmp(entry->type, "blog") == 0 &&
           strcmp(entry->created_by, user_id) == 0;
}

// Matches `decoded_url` against "<prefix>/<id><suffix>", where <id> is a
// single non-empty path segment (no '/'). On match, copies <id> into id_out
// (truncated to id_size - 1) and returns 1. `suffix` may be "" (the id is
// the last path component) or e.g. "/edit". Returns 0 on no match (id_out
// untouched).
static int match_id_route(const char *decoded_url, const char *prefix,
                           const char *suffix, char *id_out, size_t id_size) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(decoded_url, prefix, prefix_len) != 0 || decoded_url[prefix_len] != '/')
        return 0;

    const char *seg_start = decoded_url + prefix_len + 1;
    const char *slash = strchr(seg_start, '/');
    const char *seg_end;

    if (suffix[0] == '\0') {
        if (slash) return 0;
        seg_end = seg_start + strlen(seg_start);
    } else {
        if (!slash || strcmp(slash, suffix) != 0) return 0;
        seg_end = slash;
    }

    size_t seg_len = (size_t)(seg_end - seg_start);
    if (seg_len == 0 || seg_len >= id_size) return 0;

    memcpy(id_out, seg_start, seg_len);
    id_out[seg_len] = '\0';
    return 1;
}

// Theme-asset endpoints (/dashboard/api/theme-assets/*) back the banner,
// footer and logo editors' image upload/browse widget: they write straight
// into html/themes/<theme>/assets/<component>/epoch<N>/, the same
// directories the theme's own mainbanner/footer/menu-logo markup already
// references - not the content media library (html/content/posts/...,
// per-author, gallery-oriented).
//
// `key` is the theme being edited - taken explicitly from the request
// (query string), never from request_theme() (the admin's own active
// theme for this request, which need not be the theme whose banner/footer/
// logo editor is open - see theme-scoped-personalization-plan.md §4).
// `component` is restricted to the three directories this feature actually
// uses ("menu" backs the logo editor - see site_settings_logo_page());
// `epoch_str` must be one of "-1".."3". Fills dir_out (>= 256 bytes) with
// "./html/themes/<key>/assets/<component>/epoch<epoch_str>" and returns 1,
// or returns 0 (dir_out untouched) if any argument is invalid.
static int theme_assets_dir(const char *key, const char *component, const char *epoch_str,
                             char *dir_out, size_t dir_size) {
    if (!theme_key_is_valid(key)) return 0;
    if (!component || !epoch_str) return 0;
    if (strcmp(component, "mainbanner") != 0 && strcmp(component, "footer") != 0 &&
        strcmp(component, "menu") != 0)
        return 0;

    static const char *valid_epochs[] = {"-1", "0", "1", "2", "3"};
    int epoch_ok = 0;
    for (size_t i = 0; i < sizeof(valid_epochs) / sizeof(valid_epochs[0]); i++) {
        if (strcmp(epoch_str, valid_epochs[i]) == 0) { epoch_ok = 1; break; }
    }
    if (!epoch_ok) return 0;

    int n = snprintf(dir_out, dir_size, "./html/themes/%s/assets/%s/epoch%s",
                      key, component, epoch_str);
    return n > 0 && (size_t)n < dir_size;
}

// mkdir() one path component at a time - unlike media uploads (whose parent
// directory is always created up front by the "new directory" route), a
// theme-asset directory may not exist yet (there is no epoch0/ under
// assets/mainbanner/ today, and assets/footer/ doesn't exist at all).
static void mkdir_recursive(const char *dir) {
    char tmp[512];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

// Sanitizes an uploaded/targeted filename in place: strips anything but
// alphanumerics/-/./_ (spaces become '-'), same whitelist already used by
// the media library's upload handler. Also rejects a name that is empty,
// starts with '.' (hidden file / "." / ".."), or still contains "..".
static int sanitize_asset_filename(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t k = 0; in[k] && j < out_size - 1; k++) {
        char c = in[k];
        if (c == ' ') out[j++] = '-';
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
            out[j++] = c;
    }
    out[j] = '\0';
    return j > 0 && out[0] != '.' && !strstr(out, "..");
}

// Builds {"files":["name1","name2",...]} for the banner/footer editor's
// asset browser. Returns a malloc'd string, always valid JSON (an invalid
// component/epoch or an unreadable directory yields an empty list, not an
// error - this endpoint only helps the admin find a path to paste, nothing
// depends on it being exhaustive).
static char *theme_assets_list_json(const char *key, const char *component, const char *epoch_str) {
    char dir[256];
    if (!theme_assets_dir(key, component, epoch_str, dir, sizeof(dir)))
        return strdup("{\"files\":[]}");

    DIR *d = opendir(dir);
    if (!d) return strdup("{\"files\":[]}");

    char *json = strdup("{\"files\":[");
    int first = 1;
    struct dirent *entry;
    while (json && (entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *escaped = json_escape_alloc(entry->d_name);
        if (!escaped) continue;

        char *piece = render_template(first ? "\"%s\"" : ",\"%s\"", escaped);
        free(escaped);
        if (piece) { json = str_append(json, piece); first = 0; }
        free(piece);
    }
    closedir(d);

    if (!json) return strdup("{\"files\":[]}");
    json = str_append(json, "]}");
    return json ? json : strdup("{\"files\":[]}");
}

// Matches "/dashboard/api/entries/<entry_id>/blocks/<block_id>/delete". On
// match, copies both ids (truncated to their buffer sizes) and returns 1; 0
// on no match (out buffers untouched).
static int match_block_delete_route(const char *decoded_url, char *entry_id_out,
                                     size_t entry_id_size, char *block_id_out,
                                     size_t block_id_size) {
    static const char prefix[] = "/dashboard/api/entries/";
    static const char mid[]    = "/blocks/";
    static const char suffix[] = "/delete";

    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    const char *entry_start = decoded_url + prefix_len;
    const char *mid_start = strstr(entry_start, mid);
    if (!mid_start) return 0;

    const char *block_start = mid_start + (sizeof(mid) - 1);
    size_t block_len = strlen(block_start);
    size_t suffix_len = sizeof(suffix) - 1;
    if (block_len <= suffix_len || strcmp(block_start + (block_len - suffix_len), suffix) != 0)
        return 0;
    block_len -= suffix_len;

    size_t entry_len = (size_t)(mid_start - entry_start);
    if (entry_len == 0 || entry_len >= entry_id_size) return 0;
    if (block_len == 0 || block_len >= block_id_size) return 0;

    memcpy(entry_id_out, entry_start, entry_len);
    entry_id_out[entry_len] = '\0';
    memcpy(block_id_out, block_start, block_len);
    block_id_out[block_len] = '\0';
    return 1;
}

// Matches "/dashboard/settings/themes/<key>/<segment>/<epoch>" (`segment`
// is "banner" or "footer"; `epoch` is the trailing segment, possibly
// negative - "-1"). On match, copies key_out and epoch_out (each truncated
// to its buffer size) and returns 1; 0 on no match.
static int match_theme_epoch_route(const char *decoded_url, const char *segment,
                                    char *key_out, size_t key_size,
                                    char *epoch_out, size_t epoch_size) {
    static const char prefix[] = "/dashboard/settings/themes/";
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(decoded_url, prefix, prefix_len) != 0) return 0;

    const char *key_start = decoded_url + prefix_len;
    const char *slash1 = strchr(key_start, '/');
    if (!slash1) return 0;

    size_t key_len = (size_t)(slash1 - key_start);
    if (key_len == 0 || key_len >= key_size) return 0;

    size_t segment_len = strlen(segment);
    const char *segment_start = slash1 + 1;
    if (strncmp(segment_start, segment, segment_len) != 0 || segment_start[segment_len] != '/')
        return 0;

    const char *epoch_start = segment_start + segment_len + 1;
    size_t epoch_len = strlen(epoch_start);
    if (epoch_len == 0 || epoch_len >= epoch_size || strchr(epoch_start, '/')) return 0;

    memcpy(key_out, key_start, key_len);
    key_out[key_len] = '\0';
    memcpy(epoch_out, epoch_start, epoch_len);
    epoch_out[epoch_len] = '\0';
    return 1;
}

// Returns the value of a query parameter by key, or "" if not found.
static const char *get_query_param(QueryParam *params, int count, const char *key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i].key, key) == 0) return params[i].value;
    }
    return "";
}

// Extracts the url-decoded value of `key` from an
// "application/x-www-form-urlencoded" body into `out` (NUL-terminated,
// truncated to out_size - 1). Returns 1 if `key` was found, 0 otherwise
// (out is left as an empty string).
static int parse_urlencoded_field(const char *body, int body_length, const char *key,
                                   char *out, size_t out_size) {
    if (out_size == 0) return 0;
    out[0] = '\0';
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) return 0;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            char raw[8192];
            size_t val_len = (size_t)(field_end - (eq + 1));
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            char decoded[8192];
            url_decode(decoded, raw);
            strncpy(out, decoded, out_size - 1);
            out[out_size - 1] = '\0';
            return 1;
        }

        p = amp ? amp + 1 : end;
    }
    return 0;
}

// A background color field posts as two inputs - an <input type="color">
// (`color_key`) and an opacity percentage <input type="number">
// (`alpha_key`) - joined here into the single stored "#rrggbb[aa]" value via
// cms_join_hex_alpha(). Missing/unparseable fields fall back to opaque black
// (cms_join_hex_alpha()'s own invalid-input fallback) and 100% opacity.
static void parse_bg_color_field(const char *body, int body_length, const char *color_key,
                                  const char *alpha_key, char *out, size_t out_size) {
    char rgb[8] = "#000000";
    char alpha_str[8] = "100";
    parse_urlencoded_field(body, body_length, color_key, rgb, sizeof(rgb));
    parse_urlencoded_field(body, body_length, alpha_key, alpha_str, sizeof(alpha_str));
    cms_join_hex_alpha(rgb, atoi(alpha_str), out, out_size);
}

// Like parse_urlencoded_field(), but collects every value for `key` (not just
// the first), e.g. repeated "categories=..." fields from a
// <select multiple>. Each out_values[i] is malloc'd (caller must free).
// Returns the number of values found, up to max_count.
static size_t parse_urlencoded_multi(const char *body, int body_length, const char *key,
                                      char **out_values, size_t max_count) {
    size_t count = 0;
    if (!body || body_length <= 0) return 0;

    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_length;

    while (p < end && count < max_count) {
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) break;
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *field_end = amp ? amp : end;

        if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            char raw[8192];
            size_t val_len = (size_t)(field_end - (eq + 1));
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, val_len);
            raw[val_len] = '\0';

            char decoded[8192];
            url_decode(decoded, raw);
            out_values[count++] = strdup(decoded);
        }

        p = amp ? amp + 1 : end;
    }

    return count;
}

// Fills values[i] (caller-allocated, lang_count entries) with the url-decoded
// "name_<langs[i].code>" field from req's body, for
// cms_create_category()/cms_update_category(). Each values[i] is malloc'd
// and must be freed by the caller.
static void parse_category_form(HttpRequest *req, const CmsLanguageItem *langs,
                                 size_t lang_count, char **values) {
    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_link/out_order/out_enabled and values[i] (caller-allocated,
// lang_count entries) from req's body's "link"/"order"/"enabled"/
// "name_<langs[i].code>" fields, for cms_create_menu_item()/
// cms_update_menu_item(). Each values[i] is malloc'd and must be freed by
// the caller. An unchecked "enabled" checkbox sends no field at all, so its
// absence means false.
static void parse_menu_form(HttpRequest *req, char *out_link, size_t out_link_size,
                             int *out_order, bool *out_enabled,
                             const CmsLanguageItem *langs, size_t lang_count, char **values) {
    parse_urlencoded_field(req->body, req->body_length, "link", out_link, out_link_size);

    char order_str[16];
    parse_urlencoded_field(req->body, req->body_length, "order", order_str, sizeof(order_str));
    *out_order = atoi(order_str);

    char enabled_str[8];
    *out_enabled = parse_urlencoded_field(req->body, req->body_length, "enabled",
                                           enabled_str, sizeof(enabled_str)) != 0;

    for (size_t i = 0; i < lang_count; i++) {
        char field_name[40];
        snprintf(field_name, sizeof(field_name), "name_%s", langs[i].code);

        char value[256];
        parse_urlencoded_field(req->body, req->body_length, field_name, value, sizeof(value));
        values[i] = strdup(value);
    }
}

// Fills out_email/out_password/out_role (caller-allocated buffers) from req's
// body's "email"/"password"/"role" fields, for cms_create_user()/
// cms_update_user(). An absent "role" field defaults to "author" (the safer
// default for a new account).
static void parse_user_form(HttpRequest *req, char *out_email, size_t out_email_size,
                             char *out_password, size_t out_password_size,
                             char *out_role, size_t out_role_size) {
    parse_urlencoded_field(req->body, req->body_length, "email", out_email, out_email_size);
    parse_urlencoded_field(req->body, req->body_length, "password", out_password, out_password_size);

    if (!parse_urlencoded_field(req->body, req->body_length, "role", out_role, out_role_size)) {
        strncpy(out_role, "author", out_role_size - 1);
        out_role[out_role_size - 1] = '\0';
    }
}

void http_route(read_func_t read_func, void *ctx, const char *root_directory) {
    LOG_DEBUG("http_route() called");

    if (!ctx) {
        LOG_ERROR("http_route: ctx is NULL");
        return;
    }

    char *raw_request = malloc(RAW_REQUEST_SIZE);
    if (!raw_request) {
        LOG_ERROR("http_route: malloc failed");
        connection_close(ctx);
        return;
    }

    HttpRequest req;
    memset(&req, 0, sizeof(req));

    // --- Read headers ---
    int bytes_read = 0;
    while (bytes_read < (int)RAW_REQUEST_SIZE - 1) {
        int ret = read_func(ctx, raw_request + bytes_read,
                            RAW_REQUEST_SIZE - 1 - bytes_read);
        if (ret > 0) {
            bytes_read += ret;
            raw_request[bytes_read] = '\0';

            // A real HTTP/0.9 "simple request" is just "GET /path" - no
            // version token, and per spec no headers or blank-line
            // terminator ever follow it. That looks identical to an
            // ordinary request still mid-transmission until the request
            // line itself is checked for a version token: waiting for
            // "\r\n\r\n" here left a real Cello 1.x image fetch hanging
            // forever, since it never sends one for at least some requests.
            int has_versioned_line = 0;
            const char *line_end = strstr(raw_request, "\r\n");
            if (!line_end) line_end = strchr(raw_request, '\n');
            if (line_end) {
                size_t line_len = (size_t)(line_end - raw_request);
                if (line_len < 700) {
                    char first_line[700];
                    memcpy(first_line, raw_request, line_len);
                    first_line[line_len] = '\0';
                    char m[16], u[600], v[16];
                    int fields = sscanf(first_line, "%15s %599s %15s", m, u, v);
                    if (fields == 2) break;
                    has_versioned_line = (fields == 3);
                }
            }

            // Ordinary request: wait for the blank line ending the header
            // block. A bare '\n' (no '\r') is accepted too, rather than
            // waiting forever for a "\r\n\r\n" a line-ending quirk like that
            // would never actually send.
            if (strstr(raw_request, "\r\n\r\n") || strstr(raw_request, "\n\n")) break;

            // A real HTTP/1.0 request line with zero headers (or a few) and
            // no blank-line terminator at all - a real Cello 1.x fetching an
            // <img>, confirmed live: it sends a version token, sometimes a
            // header or two, and never the blank line that would normally
            // end the block. Waiting for one it will never send meant a
            // silent 30s hang and then no response whatsoever. A brief
            // pause to see whether more is actually still arriving tells
            // the two cases apart without misreading a merely-slow,
            // well-formed multi-packet request as complete. TLS clients are
            // never this old, so this only runs for plain connections.
            if (has_versioned_line && !((connection_ctx_t *)ctx)->ssl) {
                struct pollfd pfd = { .fd = ((connection_ctx_t *)ctx)->client_socket, .events = POLLIN };
                if (poll(&pfd, 1, 400) <= 0) break;
                continue;
            }

            if (bytes_read >= (int)RAW_REQUEST_SIZE - 1) {
                LOG_WARN("Headers exceed %d bytes", RAW_REQUEST_SIZE);
                send_error_response(ctx, 431, "431 Request Header Fields Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }
            continue;
        }
        if (ret == 0) {
            LOG_DEBUG("Peer closed connection while reading");
        } else {
            LOG_ERROR("read_func error: %d", ret);
        }
        goto cleanup;
    }

    raw_request[bytes_read] = '\0';
    if (bytes_read == 0) goto cleanup;

    // --- Read full body based on Content-Length ---
    {
        const char *hdr_end = strstr(raw_request, "\r\n\r\n");
        if (hdr_end) {
            int header_size = (int)(hdr_end + 4 - raw_request);
            int body_in_buffer = bytes_read - header_size;

            // Extract Content-Length from raw headers
            int content_length = 0;
            for (const char *h = raw_request; h < hdr_end; h++) {
                if (h[0] == '\r' && h[1] == '\n' &&
                    strncasecmp(h + 2, "Content-Length:", 15) == 0) {
                    const char *v = h + 17;
                    while (*v == ' ') v++;
                    content_length = atoi(v);
                    break;
                }
            }

            if (content_length > (int)MAX_BODY_SIZE) {
                LOG_WARN("Body too large: %d > %d", content_length, (int)MAX_BODY_SIZE);
                send_error_response(ctx, 413, "413 Payload Too Large", EPOCH_PRESTANDARD);
                goto cleanup;
            }

            if (content_length > 0 && content_length > body_in_buffer) {
                int total_needed = header_size + content_length;
                char *big_buf = realloc(raw_request, total_needed + 1);
                if (!big_buf) {
                    LOG_ERROR("realloc failed for body (%d bytes)", total_needed);
                    send_error_response(ctx, 500, "500 Internal Server Error", EPOCH_PRESTANDARD);
                    goto cleanup;
                }
                raw_request = big_buf;

                int remaining = content_length - body_in_buffer;
                int offset = bytes_read;
                while (remaining > 0) {
                    int ret = read_func(ctx, raw_request + offset, remaining);
                    if (ret <= 0) {
                        LOG_ERROR("Connection closed while reading body (%d remaining)", remaining);
                        goto cleanup;
                    }
                    offset += ret;
                    remaining -= ret;
                }
                bytes_read = offset;
                raw_request[bytes_read] = '\0';
            }
        }
    }

    // --- Parse request line + headers ---
    if (parse_http_request(raw_request, bytes_read, &req) != 0) {
        LOG_WARN("Malformed HTTP request");
        send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
        goto cleanup;
    }

    // A real HTTP/0.9 simple request (req.protocol left "" by
    // parse_http_request - see http_request_parser.c) legitimately has no
    // "HTTP/..." token at all; every other request must have one.
    if (req.protocol[0] != '\0' && strncmp(req.protocol, "HTTP/", 5) != 0) {
        send_error_response(ctx, 400, "400 Bad Request", EPOCH_PRESTANDARD);
        goto cleanup;
    }

    LOG_INFO("%s %s %s", req.method, req.url, req.protocol);

    // --- Determine real client IP ---
    // Honor X-Real-IP / X-Forwarded-For only from trusted reverse proxies.
    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    {
        connection_ctx_t *conn = (connection_ctx_t *)ctx;
        struct sockaddr_storage peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        char peer_ip[INET6_ADDRSTRLEN] = "unknown";

        if (getpeername(conn->client_socket, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
            if (peer_addr.ss_family == AF_INET) {
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)&peer_addr)->sin_addr,
                          peer_ip, sizeof(peer_ip));
            } else if (peer_addr.ss_family == AF_INET6) {
                inet_ntop(AF_INET6,
                          &((struct sockaddr_in6 *)&peer_addr)->sin6_addr,
                          peer_ip, sizeof(peer_ip));
            }
        }

        if (is_trusted_proxy(peer_ip)) {
            const char *real_ip  = get_header_value(&req, "X-Real-IP");
            const char *fwd      = get_header_value(&req, "X-Forwarded-For");
            if (real_ip && real_ip[0]) {
                strncpy(client_ip, real_ip, sizeof(client_ip) - 1);
            } else if (fwd && fwd[0]) {
                strncpy(client_ip, fwd, sizeof(client_ip) - 1);
                char *comma = strchr(client_ip, ',');
                if (comma) *comma = '\0';
                // trim leading space
                char *p = client_ip;
                while (*p == ' ') p++;
                if (p != client_ip) memmove(client_ip, p, strlen(p) + 1);
            } else {
                strncpy(client_ip, peer_ip, sizeof(client_ip) - 1);
            }
        } else {
            strncpy(client_ip, peer_ip, sizeof(client_ip) - 1);
        }
        client_ip[sizeof(client_ip) - 1] = '\0';
    }

    {
        const char *ua = get_header_value(&req, "User-Agent");
        LOG_DEBUG("Client IP: %s | UA: %s", client_ip, ua ? ua : "(none)");
    }

    // --- Route ---
    {
        char route[2048];
        QueryParam params[MAX_PARAMS];
        int param_count = 0;
        char url_copy[2048];
        strncpy(url_copy, req.url, sizeof(url_copy) - 1);
        url_copy[sizeof(url_copy) - 1] = '\0';
        url_parse(url_copy, route, sizeof(route), params, &param_count);

        char decoded_url[2048];
        url_decode(decoded_url, route);

        // Resolve the request's language once, before anything renders: the
        // menu reads it too, several layers down (see utils/request_lang.h).
        request_lang_set(get_header_value(&req, "Cookie"),
                          get_query_param(params, param_count, "lang"));
        request_path_set(decoded_url);
        request_user_set(get_header_value(&req, "Cookie"));
        request_theme_set(get_header_value(&req, "Cookie"),
                           get_query_param(params, param_count, "theme"));
        char content_lang[16];
        strncpy(content_lang, request_lang(), sizeof(content_lang) - 1);
        content_lang[sizeof(content_lang) - 1] = '\0';

        // Record the visit for /dashboard/analytics (no-op for bots, static
        // assets and /dashboard itself - see analytics_track_visit()'s own
        // filtering). resolve_epoch() is cheap and pure, so calling it again
        // here for tracking is fine even though every route handler below
        // calls it a second time for its own use.
        analytics_track_visit(req.method, decoded_url, get_header_value(&req, "User-Agent"),
                               client_ip, resolve_epoch(&req));

        char id[32];
        char block_id[32];
        char theme_epoch_str[8];

        if (strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0) {
            if (strcmp(decoded_url, "/") == 0) {
                int epoch = resolve_epoch(&req);

                char *body = buildHomeWebSite(epoch, content_lang);
                if (!body) LOG_ERROR("buildHomeWebSite failed for epoch %d", epoch);

                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/login") == 0) {
                int epoch = resolve_epoch(&req);

                char user_id[USER_ID_HEX_BUF_SIZE];
                const char *cookie = get_header_value(&req, "Cookie");

                if (mongodb_manager_is_ready() && validate_session_cookie(cookie, user_id) == 1) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = login(epoch, NULL);
                    char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strcmp(decoded_url, "/dashboard") == 0) {
                int epoch = resolve_epoch(&req);

                if (!mongodb_manager_is_ready()) {
                    send_error_response(ctx, 503, "503 Service Unavailable", epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    const char *cookie = get_header_value(&req, "Cookie");

                    if (validate_session_cookie(cookie, user_id) == 1) {
                        char role[USER_ROLE_BUF_SIZE];
                        if (cms_get_user_role(user_id, role, sizeof(role)) != 0) {
                            strncpy(role, "admin", sizeof(role) - 1);
                            role[sizeof(role) - 1] = '\0';
                        }

                        char *content  = dashboard(epoch, content_lang, user_id, role);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *response = build_redirect_response("/login", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = categories_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/categories/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = categories_admin_form(epoch, "", langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        if (cms_get_category_name_values(id, langs, lang_count, values) == 0) {
                            char *content  = categories_admin_form(epoch, id, langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_category_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/languages") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = languages_admin(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = menu_admin_list(epoch, content_lang);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/menu/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) values[i] = strdup("");

                        char *content  = menu_admin_form(epoch, "", "", 0, false, langs, lang_count, values, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        char **values = calloc(lang_count, sizeof(char *));
                        char link[256];
                        int order;
                        bool enabled;
                        if (cms_get_menu_item_values(id, langs, lang_count, values,
                                                      link, sizeof(link), &order, &enabled) == 0) {
                            char *content  = menu_admin_form(epoch, id, link, order, enabled,
                                                              langs, lang_count, values, NULL);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_menu_name_values_free(values, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *name     = cms_get_site_name();
                        char *content  = site_settings_general_page(epoch, name, NULL);
                        free(name);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/banner", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *values[EPOCH_COUNT];
                            cms_get_theme_banner_values(id, values);
                            char *content  = site_settings_banner_page(epoch, id, values);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                            for (int i = 0; i < EPOCH_COUNT; i++) free(values[i]);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/footer", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *values[EPOCH_COUNT];
                            cms_get_theme_footer_values(id, values);
                            char *content  = site_settings_footer_page(epoch, id, values);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                            for (int i = 0; i < EPOCH_COUNT; i++) free(values[i]);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/settings/themes", "/logo", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        if (!theme_key_is_valid(id)) {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        } else {
                            char *values[EPOCH_COUNT];
                            cms_get_theme_logo_values(id, values);
                            char *content  = site_settings_logo_page(epoch, id, values);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                            for (int i = 0; i < EPOCH_COUNT; i++) free(values[i]);
                        }
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/preview") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = site_settings_preview_page(epoch);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/fonts") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *error = get_query_param(params, param_count, "error");
                        char *content  = fonts_admin_list(epoch, error);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/settings/themes") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char **keys = NULL;
                        size_t key_count = 0;
                        list_theme_keys(&keys, &key_count);

                        char *active = cms_get_active_theme_key();

                        ThemeEntry *entries = calloc(key_count, sizeof(ThemeEntry));
                        for (size_t i = 0; i < key_count; i++) {
                            strncpy(entries[i].key, keys[i], sizeof(entries[i].key) - 1);
                            entries[i].active = strcmp(keys[i], active) == 0;
                            cms_get_theme_colors(keys[i], &entries[i].colors);
                        }

                        char *content  = site_settings_themes_page(epoch, entries, key_count);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        free(entries);
                        free(active);
                        free_theme_keys(keys, key_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/theme-assets/list") == 0) {
                int epoch = resolve_epoch(&req);
                if (epoch != EPOCH_MODERN) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *theme_key = get_query_param(params, param_count, "theme");
                        const char *component = get_query_param(params, param_count, "component");
                        const char *epoch_str = get_query_param(params, param_count, "epoch");
                        char *json = theme_assets_list_json(theme_key, component, epoch_str);
                        char *response = build_json_response(json ? json : "{\"files\":[]}");
                        free(json);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = users_admin_list(epoch, NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/analytics") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        const char *period = get_query_param(params, param_count, "period");
                        const char *date   = get_query_param(params, param_count, "date");
                        const char *from   = get_query_param(params, param_count, "from");
                        const char *to     = get_query_param(params, param_count, "to");
                        int year  = atoi(get_query_param(params, param_count, "year"));
                        int month = atoi(get_query_param(params, param_count, "month"));
                        int week  = atoi(get_query_param(params, param_count, "week"));

                        char *content  = analytics_view(epoch, period, year, month, week, date, from, to);
                        char *body     = content ? buildPageWebSite(epoch, "Boat Rudder - Dashboard", content) : NULL;
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/users/new") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char *content  = users_admin_form(epoch, "", "", "", "author", NULL);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_admin_session(ctx, &req, epoch, user_id)) {
                        char email[256];
                        char role[USER_ROLE_BUF_SIZE];
                        if (cms_get_user_values(id, email, sizeof(email), role, sizeof(role)) == 0) {
                            char *name = cms_get_user_name_by_id(id);
                            char *content  = users_admin_form(epoch, id, name ? name : "", email, role, NULL);
                            free(name);
                            char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                            char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                            free(body);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }
                    }
                }

            } else if (match_id_route(decoded_url, "/dashboard/entries", "/edit", id, sizeof(id))) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    char role[USER_ROLE_BUF_SIZE];
                    if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                        CmsLanguageItem *langs = NULL;
                        size_t lang_count = 0;
                        cms_get_languages(&langs, &lang_count);

                        CmsEntryEdit entry;
                        if (cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                            if (!can_edit_entry(role, user_id, &entry)) {
                                char *response = build_redirect_response("/dashboard", "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                CmsCategoryItem *categories = NULL;
                                size_t category_count = 0;
                                cms_get_categories(content_lang, &categories, &category_count);

                                char *content  = entry_editor_page(epoch, &entry, categories, category_count, langs, lang_count);
                                char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                                free(body);
                                send_or_error(ctx, response, req.method, epoch);

                                cms_categories_free(categories, category_count);
                            }
                        } else {
                            send_error_response(ctx, 404, "404 Not Found", epoch);
                        }

                        cms_entry_edit_free(&entry, lang_count);
                        cms_languages_free(langs, lang_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/media") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(&dirs, &dir_count);

                        char *content  = media_admin_page(epoch, dirs, dir_count, NULL, 0);
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Media", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);

                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/contents") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        const char *dir_id = get_query_param(params, param_count, "directory");
                        const char *start_str = get_query_param(params, param_count, "start");
                        const char *end_str = get_query_param(params, param_count, "end");
                        int skip = atoi(start_str);
                        int limit = atoi(end_str) - skip;
                        if (limit <= 0) limit = MEDIA_PAGE_SIZE;

                        CmsMediaItem *items = NULL;
                        size_t item_count = 0;
                        cms_get_media_items(dir_id[0] ? dir_id : NULL, skip, limit, &items, &item_count);

                        char *html = media_admin_render_items(items, item_count, epoch);
                        char *response = build_json_response(html ? html : "");
                        free(html);
                        connection_write(ctx, response, strlen(response));
                        free(response);
                        connection_close(ctx);

                        cms_media_items_free(items, item_count);
                    }
                }

            } else if (strcmp(decoded_url, "/dashboard/api/media/modal") == 0) {
                int epoch = resolve_epoch(&req);

                if (epoch != EPOCH_MODERN) {
                    send_simple(ctx, "403 Forbidden", "Forbidden");
                } else {
                    char user_id[USER_ID_HEX_BUF_SIZE];
                    if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                        CmsMediaDirectory *dirs = NULL;
                        size_t dir_count = 0;
                        cms_get_media_directories(&dirs, &dir_count);

                        char *html = media_admin_modal(epoch, dirs, dir_count, NULL, 0);
                        if (html) {
                            char header_buf[128];
                            snprintf(header_buf, sizeof(header_buf),
                                     "Content-Type: text/html; charset=UTF-8\r\n"
                                     "Content-Length: %zu\r\n", strlen(html));
                            char response_buf[256];
                            snprintf(response_buf, sizeof(response_buf),
                                     "HTTP/1.1 200 OK\r\n%s\r\n", header_buf);
                            connection_write(ctx, response_buf, strlen(response_buf));
                            connection_write(ctx, html, strlen(html));
                            free(html);
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "Error");
                        }
                        connection_close(ctx);

                        cms_media_directories_free(dirs, dir_count);
                    }
                }

            } else if (strcmp(decoded_url, "/logout") == 0) {
                int epoch = resolve_epoch(&req);

                char token[SESSION_TOKEN_BUF_SIZE];
                const char *cookie = get_header_value(&req, "Cookie");
                if (mongodb_manager_is_ready() && extract_session_token(cookie, token, sizeof(token))) {
                    destroy_session(token);
                }

                char clear_cookie[128];
                build_session_clear_cookie_header(clear_cookie, sizeof(clear_cookie));

                char *response = build_redirect_response("/", clear_cookie, epoch);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/language/set") == 0) {
                // Stores the choice and bounces back. A plain GET on purpose:
                // it has to work from a nav-bar link on a browser with no
                // JavaScript and no form support to speak of.
                int epoch = resolve_epoch(&req);

                // url_parse() leaves query values percent-encoded, so "%2Fblog"
                // has to be decoded before it can be recognised as a path.
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));

                char safe_return[512];
                language_sanitize_return(return_raw, safe_return, sizeof(safe_return));

                char code[16];
                char extra[192] = "";
                if (request_lang_validate(get_query_param(params, param_count, "code"),
                                           code, sizeof(code))) {
                    // One year: the choice should outlive the browsing session.
                    snprintf(extra, sizeof(extra),
                             "Set-Cookie: lang=%s; Path=/; Max-Age=31536000; SameSite=Lax\r\n",
                             code);
                }

                char *abs_return = absolute_location(ctx, &req, safe_return);
                char *response = build_redirect_response(abs_return ? abs_return : safe_return,
                                                          extra, epoch);
                free(abs_return);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/theme/set") == 0) {
                // Mirrors /language/set exactly, including the plain-GET
                // rationale: a nav-bar link with no JS/form needed. Epoch 3
                // only offers this from the navbar (see menu.c's
                // theme_selector()), but the route itself doesn't check
                // epoch - a hand-typed link works from any browser.
                int epoch = resolve_epoch(&req);

                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));

                char safe_return[512];
                language_sanitize_return(return_raw, safe_return, sizeof(safe_return));

                char extra[192] = "";
                const char *key = get_query_param(params, param_count, "key");
                if (theme_key_is_valid(key)) {
                    snprintf(extra, sizeof(extra),
                             "Set-Cookie: theme=%s; Path=/; Max-Age=31536000; SameSite=Lax\r\n",
                             key);
                }

                char *abs_return = absolute_location(ctx, &req, safe_return);
                char *response = build_redirect_response(abs_return ? abs_return : safe_return,
                                                          extra, epoch);
                free(abs_return);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/language") == 0) {
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char *content = language_page(epoch, return_raw);
                char *body    = content ? buildPageWebSite(epoch, "Language", content) : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strcmp(decoded_url, "/theme") == 0) {
                // Mirrors /language exactly - the epoch 1/2 nav bar link's
                // full-page fallback (menu.c's theme_selector()), listing
                // every theme (theme_page.c).
                int epoch = resolve_epoch(&req);
                char return_raw[512];
                url_decode(return_raw, get_query_param(params, param_count, "return"));
                char *content = theme_page(epoch, return_raw);
                char *body    = content ? buildPageWebSite(epoch, "Theme", content) : NULL;
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/page/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                int page = atoi(get_query_param(params, param_count, "page"));
                serve_cms_entry(ctx, decoded_url + 6, "page", content_lang, req.method, epoch, NULL, page);

            } else if (strncmp(decoded_url, "/gallery/", 9) == 0 && decoded_url[9] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *gallery_id = decoded_url + 9;
                const char *img_str = get_query_param(params, param_count, "img");
                int img_index = img_str[0] ? atoi(img_str) : 0;

                if (!mongodb_manager_is_ready() || strlen(gallery_id) != 24) {
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    CmsMediaGallery gallery;
                    if (!cms_get_media_gallery(gallery_id, &gallery)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                    // The "< Back" link goes to the entry the gallery belongs
                    // to, named by its title, so a reader who followed a link
                    // into the gallery can find their way back to what they
                    // were reading - not just to wherever they happened to
                    // click in from. Falls back to the browser history when
                    // the entry cannot be resolved (e.g. a gallery left
                    // without a parent after the entry was deleted).
                    char back_url[280] = "";
                    char back_title[256] = "";
                    bool has_backlink = cms_get_entry_backlink(gallery.entry_id, content_lang,
                                                                back_url, sizeof(back_url),
                                                                back_title, sizeof(back_title));
                    const char *back_href = has_backlink ? back_url : "javascript:history.back()";
                    char back_label[300];
                    if (has_backlink && back_title[0])
                        snprintf(back_label, sizeof(back_label), "Back to %s", back_title);
                    else
                        snprintf(back_label, sizeof(back_label), "Back");

                    if (epoch >= 3) {
                        char *item_tpl_path = generate_url_theme("elements/gallery/gallery-page-item_epoch%d.html", epoch);
                        char *item_tpl = item_tpl_path ? read_file_to_string(item_tpl_path) : NULL;
                        free(item_tpl_path);

                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!item_tpl || !page_tpl) {
                            free(item_tpl); free(page_tpl);
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *items_html = strdup("");
                            for (size_t i = 0; items_html && i < gallery.url_count; i++) {
                                char *full  = image_url_variant(gallery.urls[i], "_full");
                                char *small = image_url_variant(gallery.urls[i], "_small");
                                char *item  = (full && small) ? render_template(item_tpl, full, small) : NULL;
                                if (item) items_html = str_append(items_html, item);
                                free(item); free(full); free(small);
                            }
                            free(item_tpl);

                            char *page_html = items_html ? render_template(page_tpl, back_href, back_label, items_html) : NULL;
                            free(items_html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    } else {
                        // Epochs -1/0/1/2: paginated gallery
                        int count = (int)gallery.url_count;
                        int cur = (img_index >= 0 && img_index < count) ? img_index : 0;
                        int prev_idx = (cur - 1 + count) % count;
                        int next_idx = (cur + 1) % count;

                        // The "< Back" link is fixed markup baked into the page
                        // shell itself (gallery-page_epoch<N>.html) - it never
                        // varies and takes no argument, so it does not need its
                        // own template. Epoch 3's shell already did this; the
                        // others used to render a separate gallery-page-back
                        // file and prepend it here, which duplicated the link
                        // on epoch 0, whose shell already carried its own copy.
                        char *page_tpl_path = generate_url_theme("elements/gallery/gallery-page_epoch%d.html", epoch);
                        char *page_tpl = page_tpl_path ? read_file_to_string(page_tpl_path) : NULL;
                        free(page_tpl_path);

                        if (!page_tpl) {
                            send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                        } else {
                            char *html = strdup("");

                            if (epoch <= 0) {
                                // Text-only browsers (epoch 0) and WAP phones
                                // (WML) can't show any of the actual photos -
                                // "[Image 1] [Image 2] ..." was markup with
                                // nothing behind it to view. A QR code that
                                // resolves to this same gallery gives the
                                // reader a real way to see it: scan it with
                                // the phone that's sitting right next to the
                                // old machine. Mirrors the previous site.
                                char gallery_path[300];
                                snprintf(gallery_path, sizeof(gallery_path), "/gallery/%s", gallery_id);

                                char gallery_url[600];
                                if (public_url[0]) {
                                    snprintf(gallery_url, sizeof(gallery_url), "%s%s", public_url, gallery_path);
                                } else {
                                    char *abs = absolute_location(ctx, &req, gallery_path);
                                    snprintf(gallery_url, sizeof(gallery_url), "%s", abs ? abs : gallery_path);
                                    free(abs);
                                }

                                char *qr_tpl_path = generate_url_theme("elements/gallery/gallery-page-qr_epoch%d.html", epoch);
                                char *qr_tpl = qr_tpl_path ? read_file_to_string(qr_tpl_path) : NULL;
                                free(qr_tpl_path);

                                if (qr_tpl && epoch == EPOCH_WML) {
                                    char qr_dir[512];
                                    snprintf(qr_dir, sizeof(qr_dir), "%s/content/qr", root_directory);
                                    mkdir(qr_dir, 0755);

                                    char qr_fs_path[512];
                                    snprintf(qr_fs_path, sizeof(qr_fs_path), "%s/content/qr/gallery-%s.wbmp",
                                             root_directory, gallery_id);
                                    char qr_web_path[280];
                                    snprintf(qr_web_path, sizeof(qr_web_path), "/content/qr/gallery-%s.wbmp", gallery_id);

                                    if (generate_qr_wbmp(gallery_url, qr_fs_path) == 0) {
                                        char *line = render_template(qr_tpl, qr_web_path, gallery_url, gallery_url);
                                        if (line) html = str_append(html, line);
                                        free(line);
                                    }
                                } else if (qr_tpl) {
                                    char *qr_text = generate_qr_halfblock_text(gallery_url);
                                    if (qr_text) {
                                        char *line = render_template(qr_tpl, qr_text, gallery_url, gallery_url);
                                        if (line) html = str_append(html, line);
                                        free(line);
                                    }
                                    free(qr_text);
                                }
                                free(qr_tpl);
                            } else {
                                char *main_tpl_path  = generate_url_theme("elements/gallery/gallery-page-main_epoch%d.html", epoch);
                                char *main_tpl       = main_tpl_path ? read_file_to_string(main_tpl_path) : NULL;
                                // One wrapper, %s in the middle - not a start/end
                                // pair. A template is a hole to fill, not two
                                // halves of one to reassemble by hand.
                                char *strip_path     = generate_url_theme("elements/gallery/gallery-page-thumbstrip_epoch%d.html", epoch);
                                char *strip_tpl       = strip_path ? read_file_to_string(strip_path) : NULL;
                                char *thumb_tpl_path = generate_url_theme("elements/gallery/gallery-page-thumb_epoch%d.html", epoch);
                                char *thumb_tpl      = thumb_tpl_path ? read_file_to_string(thumb_tpl_path) : NULL;
                                free(main_tpl_path); free(strip_path); free(thumb_tpl_path);

                                if (main_tpl && strip_tpl && thumb_tpl) {
                                    // The main image is the point of this page.
                                    // Epoch 2 gets `_half` (capped at 1024px,
                                    // keeps the source format); epoch 1 predates
                                    // progressive JPEG entirely and can only
                                    // show GIF, so it gets `_medium` instead,
                                    // rewritten to .gif like every other epoch-1
                                    // image on the site.
                                    char *main_url = (epoch == EPOCH_EARLY)
                                        ? image_url_variant(gallery.urls[cur], "_medium")
                                        : image_url_variant(gallery.urls[cur], "_half");
                                    if (epoch == EPOCH_EARLY && main_url) {
                                        char *dot = strrchr(main_url, '.');
                                        if (dot) strcpy(dot, ".gif");
                                    }

                                    // The width goes in pixels, read from the
                                    // file. A percentage on <img> is HTML 4.0
                                    // and these browsers draw it zero-wide;
                                    // they also never reflow, so the image is
                                    // capped to the width of the screen they
                                    // ran on. Narrower images keep their own
                                    // size rather than being blown up.
                                    char *main_block = render_template(main_tpl,
                                        gallery_id, prev_idx, main_url ? main_url : "",
                                        gallery_id, next_idx);
                                    free(main_url);
                                    if (main_block) { html = str_append(html, main_block); free(main_block); }

                                    // No table, no fixed columns: each thumbnail
                                    // is just an inline <a><img>, so the browser
                                    // wraps as many as fit the screen it's
                                    // actually running on rather than however
                                    // many happened to fit whatever screen a
                                    // fixed column count was chosen for.
                                    char *thumbs_html = strdup("");
                                    for (int i = 0; i < count && thumbs_html; i++) {
                                        char *t = (epoch == 1)
                                            ? image_url_variant(gallery.urls[i], "_micro")
                                            : image_url_variant(gallery.urls[i], "_small");
                                        if (epoch == 1 && t) {
                                            char *dot = strrchr(t, '.'); if (dot) strcpy(dot, ".gif");
                                        }
                                        int border = (i == cur) ? 3 : 1;
                                        char *thumb = render_template(thumb_tpl, gallery_id, i, t ? t : "", border);
                                        free(t);

                                        if (thumb) { thumbs_html = str_append(thumbs_html, thumb); free(thumb); }
                                    }

                                    char *strip = thumbs_html ? render_template(strip_tpl, thumbs_html) : NULL;
                                    free(thumbs_html);
                                    if (strip) { html = str_append(html, strip); free(strip); }
                                }
                                free(main_tpl); free(strip_tpl); free(thumb_tpl);
                            }

                            char *page_html = html ? render_template(page_tpl, back_href, back_label, html) : NULL;
                            free(html);
                            free(page_tpl);

                            if (page_html) {
                                char *response = build_epoch_response(page_html, "", epoch);
                                send_or_error(ctx, response, req.method, epoch);
                            } else {
                                send_error_response(ctx, 500, "500 Internal Server Error", epoch);
                            }
                        }
                    }
                    } // close the back-link scope
                    cms_media_gallery_free(&gallery);
                }

            } else if (strcmp(decoded_url, "/blog") == 0) {
                int epoch = resolve_epoch(&req);

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);
                char *cat_menu = category_menu_render(cats, cat_count, NULL, epoch);
                cms_categories_free(cats, cat_count);

                char *content  = blog_list(epoch, content_lang);
                char *body     = buildBlogListWebSiteAtUrl(epoch, "Boat Rudder - Blog", content, "/blog", cat_menu);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (strncmp(decoded_url, "/blog/category/", 15) == 0 && decoded_url[15] != '\0') {
                int epoch = resolve_epoch(&req);
                const char *cat_slug = decoded_url + 15;

                CmsCategoryItem *cats = NULL;
                size_t cat_count = 0;
                cms_get_categories(content_lang, &cats, &cat_count);

                char *cat_id   = NULL;
                char *cat_name = NULL;
                for (size_t i = 0; i < cat_count; i++) {
                    char *slug = slugify(cats[i].name);
                    if (slug && strcmp(slug, cat_slug) == 0) {
                        cat_id   = strdup(cats[i].id);
                        cat_name = strdup(cats[i].name);
                    }
                    free(slug);
                    if (cat_id) break;
                }

                if (!cat_id) {
                    cms_categories_free(cats, cat_count);
                    send_error_response(ctx, 404, "404 Not Found", epoch);
                } else {
                    char *cat_menu = category_menu_render(cats, cat_count, cat_slug, epoch);
                    cms_categories_free(cats, cat_count);

                    char *content = blog_list_category(epoch, content_lang, cat_id);
                    free(cat_id);

                    char page_title[256];
                    snprintf(page_title, sizeof(page_title), "Blog - %s", cat_name ? cat_name : cat_slug);
                    free(cat_name);

                    char *body     = buildBlogListWebSiteAtUrl(epoch, page_title, content, "/blog", cat_menu);
                    char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }

            } else if (strncmp(decoded_url, "/blog/", 6) == 0 && decoded_url[6] != '\0') {
                int epoch = resolve_epoch(&req);
                int page = atoi(get_query_param(params, param_count, "page"));
                serve_cms_entry(ctx, decoded_url + 6, "blog", content_lang, req.method, epoch, NULL, page);

            } else {
                const char *ims = get_header_value(&req, "If-Modified-Since");
                int status = serve_static_file(ctx, root_directory, decoded_url, ims);
                if (status != 0) {
                    const char *status_line = status == 404 ? "404 Not Found"
                                             : status == 403 ? "403 Forbidden"
                                             : "500 Internal Server Error";
                    send_error_response(ctx, status, status_line, resolve_epoch(&req));
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/login") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *content  = login(epoch, NULL);
                char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                free(body);
                send_or_error(ctx, response, req.method, epoch);

            } else if (!mongodb_manager_is_ready()) {
                send_error_response(ctx, 503, "503 Service Unavailable", epoch);

            } else {
                char email[256];
                char password[256];
                parse_urlencoded_field(req.body, req.body_length, "user", email, sizeof(email));
                parse_urlencoded_field(req.body, req.body_length, "password", password, sizeof(password));

                char *user_id = auth_login_user(email, password);
                char *response = NULL;

                if (user_id) {
                    char *token = generate_session_token();
                    if (token && create_session(user_id, token, session_ttl_seconds) == 0) {
                        char cookie_header[256];
                        build_session_cookie_header(token, session_ttl_seconds, cookie_header, sizeof(cookie_header));
                        response = build_redirect_response("/dashboard", cookie_header, epoch);
                    }
                    free(token);
                    free(user_id);
                    send_or_error(ctx, response, req.method, epoch);
                } else {
                    char *content  = login(epoch, "Invalid email or password.");
                    char *body     = buildPageWebSite(epoch, "Boat Rudder - Login", content);
                    response       = body ? build_epoch_response(body, "", epoch) : NULL;
                    free(body);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/categories/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_create_category(langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    parse_category_form(&req, langs, lang_count, values);

                    cms_update_category(id, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_category_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/categories", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_category(id);

                    char *response = build_redirect_response("/dashboard/categories", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/menu/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_create_menu_item(link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    char **values = calloc(lang_count, sizeof(char *));
                    char link[256];
                    int order;
                    bool enabled;
                    parse_menu_form(&req, link, sizeof(link), &order, &enabled, langs, lang_count, values);

                    cms_update_menu_item(id, link, order, enabled, langs, lang_count, values);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);

                    cms_menu_name_values_free(values, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/menu", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_menu_item(id);

                    char *response = build_redirect_response("/dashboard/menu", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/entries/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    const char *type = strcmp(role, "author") == 0 ? "blog" : "page";
                    char *new_id = cms_create_entry(user_id, type);
                    char *response;
                    if (new_id) {
                        char location[64];
                        snprintf(location, sizeof(location), "/dashboard/entries/%s/edit", new_id);
                        response = build_redirect_response(location, "", epoch);
                    } else {
                        response = build_redirect_response("/dashboard", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                    free(new_id);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/entries", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_delete_entry(id);

                    char *response = build_redirect_response("/dashboard", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/meta", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char link[256];
                        char type[16];
                        parse_urlencoded_field(req.body, req.body_length, "link", link, sizeof(link));
                        if (strcmp(role, "author") == 0) {
                            strcpy(type, "blog");
                        } else if (!parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type))) {
                            strcpy(type, "page");
                        }

                        char enabled_str[8];
                        bool enabled = parse_urlencoded_field(req.body, req.body_length, "enabled",
                                                               enabled_str, sizeof(enabled_str)) != 0;

                        char *category_ids[CATEGORY_LIST_LIMIT];
                        size_t category_count = parse_urlencoded_multi(req.body, req.body_length, "categories",
                                                                         category_ids, CATEGORY_LIST_LIMIT);

                        if (cms_update_entry_meta(id, link, type, enabled, category_ids, category_count) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < category_count; i++) free(category_ids[i]);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/header", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char image_url[1024];
                        char date[16];
                        char hide_author_str[8];
                        parse_urlencoded_field(req.body, req.body_length, "image_url", image_url, sizeof(image_url));
                        parse_urlencoded_field(req.body, req.body_length, "date", date, sizeof(date));
                        // Unchecked checkboxes are not submitted at all, so an
                        // absent field means "show the author".
                        parse_urlencoded_field(req.body, req.body_length, "hide_author",
                                               hide_author_str, sizeof(hide_author_str));
                        bool hide_author = (strcmp(hide_author_str, "1") == 0);

                        char **title_values   = calloc(lang_count, sizeof(char *));
                        char **summary_values = calloc(lang_count, sizeof(char *));
                        for (size_t i = 0; i < lang_count; i++) {
                            char field[40], value[1024];

                            snprintf(field, sizeof(field), "title_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            title_values[i] = strdup(value);

                            snprintf(field, sizeof(field), "summary_%s", langs[i].code);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            summary_values[i] = strdup(value);
                        }

                        if (cms_update_entry_header(id, image_url, date, hide_author, langs, lang_count,
                                                     title_values, summary_values) == 0) {
                            response = build_json_response("{\"ok\":true}");
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (size_t i = 0; i < lang_count; i++) {
                            free(title_values[i]);
                            free(summary_values[i]);
                        }
                        free(title_values);
                        free(summary_values);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/content", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                        send_or_error(ctx, response, req.method, epoch);
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char count_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "content_count", count_str, sizeof(count_str));
                        int block_count = atoi(count_str);
                        if (block_count < 0) block_count = 0;
                        if (block_count > 200) block_count = 200;

                        CmsContentBlockEdit *blocks = calloc((size_t)block_count, sizeof(CmsContentBlockEdit));
                        for (int i = 0; i < block_count; i++) {
                            char field[48], value[8192];

                            snprintf(field, sizeof(field), "content_%d_id", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].id = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_type", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].type = strdup(value);

                            snprintf(field, sizeof(field), "content_%d_order", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].order = atoi(value);

                            snprintf(field, sizeof(field), "content_%d_extra_data", i);
                            parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                            blocks[i].extra_data = strdup(value);

                            blocks[i].text_values = calloc(lang_count, sizeof(char *));
                            for (size_t j = 0; j < lang_count; j++) {
                                snprintf(field, sizeof(field), "content_%d_text_%s", i, langs[j].code);
                                parse_urlencoded_field(req.body, req.body_length, field, value, sizeof(value));
                                blocks[i].text_values[j] = strdup(value);
                            }
                        }

                        if (cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count) == 0) {
                            for (int gi = 0; gi < block_count; gi++) {
                                if (blocks[gi].type && strcmp(blocks[gi].type, "gallery") == 0 &&
                                    blocks[gi].text_values && blocks[gi].text_values[0] &&
                                    blocks[gi].text_values[0][0]) {
                                    char gal_id[25];
                                    const char *existing = blocks[gi].extra_data;
                                    if (cms_upsert_media_gallery(
                                            (existing && strlen(existing) == 24) ? existing : NULL,
                                            id, blocks[gi].text_values[0], gal_id) == 0) {
                                        free(blocks[gi].extra_data);
                                        blocks[gi].extra_data = strdup(gal_id);
                                        cms_update_entry_content(id, langs, lang_count, blocks, (size_t)block_count);
                                    }
                                }
                            }
                            // Hand back the (possibly freshly minted) block ids
                            // so the editor's data-block-id attributes stay in
                            // sync with what is now stored.
                            size_t ids_size = 32 + (size_t)block_count * 28;
                            char *ids_json = malloc(ids_size);
                            size_t off = (size_t)snprintf(ids_json, ids_size, "{\"ok\":true,\"ids\":[");
                            for (int i = 0; i < block_count; i++)
                                off += (size_t)snprintf(ids_json + off, ids_size - off, "%s\"%s\"",
                                                        i ? "," : "",
                                                        blocks[i].id ? blocks[i].id : "");
                            snprintf(ids_json + off, ids_size - off, "]}");
                            response = build_json_response(ids_json);
                            free(ids_json);
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"update failed\"}", "400 Bad Request");
                        }
                        send_or_error(ctx, response, req.method, epoch);

                        for (int i = 0; i < block_count; i++) {
                            free(blocks[i].id);
                            free(blocks[i].type);
                            free(blocks[i].extra_data);
                            for (size_t j = 0; j < lang_count; j++) free(blocks[i].text_values[j]);
                            free(blocks[i].text_values);
                        }
                        free(blocks);
                    }

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_block_delete_route(decoded_url, id, sizeof(id), block_id, sizeof(block_id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else if (cms_remove_entry_content_block(id, block_id) == 0) {
                        response = build_json_response("{\"ok\":true}");
                    } else {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"remove failed\"}", "400 Bad Request");
                    }
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/api/entries", "/blocks", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                char role[USER_ROLE_BUF_SIZE];
                if (require_dashboard_session_role(ctx, &req, epoch, user_id, role, sizeof(role))) {
                    CmsLanguageItem *langs = NULL;
                    size_t lang_count = 0;
                    cms_get_languages(&langs, &lang_count);

                    CmsEntryEdit entry;
                    char *response = NULL;
                    if (!cms_get_entry_for_edit(id, langs, lang_count, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"not found\"}", "404 Not Found");
                    } else if (!can_edit_entry(role, user_id, &entry)) {
                        response = build_json_response_status("{\"ok\":false,\"error\":\"forbidden\"}", "403 Forbidden");
                    } else {
                        char type[16];
                        char order_str[16];
                        parse_urlencoded_field(req.body, req.body_length, "type", type, sizeof(type));
                        parse_urlencoded_field(req.body, req.body_length, "order", order_str, sizeof(order_str));
                        int order = atoi(order_str);

                        char new_block_id[32];
                        if (cms_add_entry_content_block(id, type, order, new_block_id, sizeof(new_block_id)) == 0) {
                            CmsContentBlockEdit block = {0};
                            block.id = new_block_id;
                            block.type = type;
                            block.order = order;
                            block.extra_data = (char *)"";
                            block.text_values = calloc(lang_count, sizeof(char *));
                            for (size_t i = 0; i < lang_count; i++) block.text_values[i] = (char *)"";

                            char *html = entry_editor_render_block(&block, langs, lang_count, epoch);
                            free(block.text_values);

                            char *html_json = html ? json_escape_alloc(html) : NULL;
                            free(html);

                            if (html_json) {
                                char *json_body = render_template(
                                    "{\"ok\":true,\"block_id\":\"%s\",\"html\":\"%s\"}", new_block_id, html_json);
                                response = json_body ? build_json_response(json_body) : NULL;
                                free(json_body);
                            }
                            free(html_json);
                        } else {
                            response = build_json_response_status("{\"ok\":false,\"error\":\"create failed\"}", "400 Bad Request");
                        }
                    }

                    if (!response)
                        response = build_json_response_status("{\"ok\":false,\"error\":\"internal error\"}", "500 Internal Server Error");
                    send_or_error(ctx, response, req.method, epoch);

                    cms_entry_edit_free(&entry, lang_count);
                    cms_languages_free(langs, lang_count);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/settings") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[128];
                    parse_urlencoded_field(req.body, req.body_length, "site_name", name, sizeof(name));

                    if (name[0] && cms_update_site_name(name) == 0) {
                        char *response = build_redirect_response("/dashboard/settings", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = site_settings_general_page(epoch, name,
                            name[0] ? "No se pudo guardar el nombre del sitio." : "El nombre del sitio no puede estar vacio.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/themes", "/activate", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_set_active_theme(id);
                    char *response = build_redirect_response("/dashboard/settings/themes", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/themes", "/colors", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        CmsThemeColors colors = {0};
                        parse_bg_color_field(req.body, req.body_length, "navbar-background",
                                             "navbar-background-alpha",
                                             colors.navbar_background, sizeof(colors.navbar_background));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-normal",
                                                colors.navbar_menu_normal, sizeof(colors.navbar_menu_normal));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-hover",
                                                colors.navbar_menu_hover, sizeof(colors.navbar_menu_hover));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-menu-active",
                                                colors.navbar_menu_active, sizeof(colors.navbar_menu_active));
                        parse_urlencoded_field(req.body, req.body_length, "navbar-logo",
                                                colors.navbar_logo, sizeof(colors.navbar_logo));
                        parse_bg_color_field(req.body, req.body_length, "body-background",
                                             "body-background-alpha",
                                             colors.body_background, sizeof(colors.body_background));
                        parse_urlencoded_field(req.body, req.body_length, "body-background-epoch1",
                                                colors.body_background_epoch1,
                                                sizeof(colors.body_background_epoch1));
                        parse_bg_color_field(req.body, req.body_length, "home-content-background",
                                             "home-content-background-alpha",
                                             colors.home_content_background, sizeof(colors.home_content_background));
                        parse_urlencoded_field(req.body, req.body_length, "home-content-text",
                                                colors.home_content_text, sizeof(colors.home_content_text));
                        parse_bg_color_field(req.body, req.body_length, "blog-list-item-background",
                                             "blog-list-item-background-alpha",
                                             colors.blog_list_item_background, sizeof(colors.blog_list_item_background));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-border",
                                                colors.blog_list_item_border, sizeof(colors.blog_list_item_border));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-author",
                                                colors.blog_list_item_author, sizeof(colors.blog_list_item_author));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-categories",
                                                colors.blog_list_item_categories, sizeof(colors.blog_list_item_categories));
                        parse_urlencoded_field(req.body, req.body_length, "blog-list-item-date",
                                                colors.blog_list_item_date, sizeof(colors.blog_list_item_date));
                        parse_urlencoded_field(req.body, req.body_length, "footer-logo",
                                                colors.footer_logo, sizeof(colors.footer_logo));
                        parse_bg_color_field(req.body, req.body_length, "footer-logo-background",
                                             "footer-logo-background-alpha",
                                             colors.footer_logo_background, sizeof(colors.footer_logo_background));

                        char logo_font[128] = "";
                        parse_urlencoded_field(req.body, req.body_length, "logo-font",
                                                logo_font, sizeof(logo_font));

                        cms_update_theme_colors(id, &colors);
                        cms_update_theme_logo_font(id, logo_font);
                        char *response = build_redirect_response("/dashboard/settings/themes", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "banner", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char html[8192];
                        parse_urlencoded_field(req.body, req.body_length, "html", html, sizeof(html));

                        cms_update_theme_banner(id, atoi(theme_epoch_str), html);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/banner", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "footer", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char html[8192];
                        parse_urlencoded_field(req.body, req.body_length, "html", html, sizeof(html));

                        cms_update_theme_footer(id, atoi(theme_epoch_str), html);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/footer", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_theme_epoch_route(decoded_url, "logo", id, sizeof(id),
                                            theme_epoch_str, sizeof(theme_epoch_str))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (!theme_key_is_valid(id)) {
                        send_error_response(ctx, 404, "404 Not Found", epoch);
                    } else {
                        char html[8192];
                        parse_urlencoded_field(req.body, req.body_length, "html", html, sizeof(html));

                        cms_update_theme_logo(id, atoi(theme_epoch_str), html);
                        char redirect_to[128];
                        snprintf(redirect_to, sizeof(redirect_to), "/dashboard/settings/themes/%s/logo", id);
                        char *response = build_redirect_response(redirect_to, "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/languages/add") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char code[16];
                    parse_urlencoded_field(req.body, req.body_length, "code", code, sizeof(code));

                    if (cms_add_language(code) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "Idioma desconocido o ya agregado.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/default", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    cms_set_default_language(id);

                    char *response = build_redirect_response("/dashboard/languages", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/languages", "/remove", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    if (cms_remove_language(id) == 0) {
                        char *response = build_redirect_response("/dashboard/languages", "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        char *content  = languages_admin(epoch, "No se puede eliminar el idioma predeterminado o el ultimo idioma.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        char *response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                        send_or_error(ctx, response, req.method, epoch);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 && strcmp(decoded_url, "/dashboard/users/new") == 0) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[256], email[256], password[256], role[USER_ROLE_BUF_SIZE];
                    parse_urlencoded_field(req.body, req.body_length, "name", name, sizeof(name));
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));

                    char *response;
                    if (cms_create_user(name, email, password, role) == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        char *content  = users_admin_form(epoch, "", name, email, role,
                                              "No se pudo crear el usuario. Verifica el email y la contrasena.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/edit", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char name[256], email[256], password[256], role[USER_ROLE_BUF_SIZE];
                    parse_urlencoded_field(req.body, req.body_length, "name", name, sizeof(name));
                    parse_user_form(&req, email, sizeof(email), password, sizeof(password), role, sizeof(role));

                    char *response;
                    if (strcmp(id, user_id) == 0 && strcmp(role, "admin") != 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_form(epoch, id, name, email, role,
                                              "No puedes quitarte el unico rol de administrador.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_update_user(id, name, email, role, password) == 0) {
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    } else {
                        char *content  = users_admin_form(epoch, id, name, email, role,
                                              "No se pudo actualizar el usuario. Verifica el email.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/users", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);

            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char target_role[USER_ROLE_BUF_SIZE];
                    char *response;

                    if (strcmp(id, user_id) == 0) {
                        char *content  = users_admin_list(epoch, "No puedes eliminar tu propia cuenta.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else if (cms_get_user_role(id, target_role, sizeof(target_role)) == 0 &&
                               strcmp(target_role, "admin") == 0 && cms_count_admins() == 1) {
                        char *content  = users_admin_list(epoch, "No se puede eliminar el ultimo administrador.");
                        char *body     = buildPageWebSite(epoch, "Boat Rudder - Dashboard", content);
                        response = body ? build_epoch_response(body, "", epoch) : NULL;
                        free(body);
                    } else {
                        cms_delete_user(id);
                        response = build_redirect_response("/dashboard/users", "", epoch);
                    }
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        // ---- Media admin POST routes ----

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char name[256];
                    parse_urlencoded_field(req.body, req.body_length, "newpath", name, sizeof(name));

                    size_t nlen = strlen(name);
                    int valid = nlen >= 3 && nlen <= 60;
                    for (size_t i = 0; valid && i < nlen; i++) {
                        char c = name[i];
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_'))
                            valid = 0;
                    }
                    if (strstr(name, "..")) valid = 0;

                    if (!valid) {
                        send_simple(ctx, "400 Bad Request", "Invalid directory name (3-60 chars, a-z 0-9 _ -)");
                    } else {
                        char username[64] = {0};
                        cms_get_username_by_id(user_id, username, sizeof(username));

                        char dirpath[512];
                        snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, name);

                        struct stat st;
                        if (stat(dirpath, &st) == -1) {
                            char parent_path[512];
                            snprintf(parent_path, sizeof(parent_path), "./html/content/posts/%s", username);
                            mkdir(parent_path, 0775);
                            mkdir(dirpath, 0775);
                        }

                        char new_id[25];
                        if (cms_create_media_directory(name, "posts", user_id, new_id) == 0) {
                            CmsMediaDirectory dir;
                            if (cms_get_media_directory_by_id(new_id, &dir)) {
                                strncpy(dir.author_name, username, sizeof(dir.author_name) - 1);
                                char *html = media_admin_render_directory_item(&dir, epoch);
                                if (html) {
                                    char hdr[128];
                                    snprintf(hdr, sizeof(hdr),
                                             "Content-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\n",
                                             strlen(html));
                                    char resp[256];
                                    snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\n%s\r\n", hdr);
                                    connection_write(ctx, resp, strlen(resp));
                                    connection_write(ctx, html, strlen(html));
                                    free(html);
                                } else {
                                    send_simple(ctx, "500 Internal Server Error", "Render error");
                                }
                            } else {
                                send_simple(ctx, "500 Internal Server Error", "DB lookup error");
                            }
                        } else {
                            send_simple(ctx, "500 Internal Server Error", "DB insert error");
                        }
                        connection_close(ctx);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/rename") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char dir_id[32], oldname[256], newname[256];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));
                    parse_urlencoded_field(req.body, req.body_length, "oldname", oldname, sizeof(oldname));
                    parse_urlencoded_field(req.body, req.body_length, "newname", newname, sizeof(newname));

                    char username[64] = {0};
                    cms_get_username_by_id(user_id, username, sizeof(username));

                    char old_path[512], new_path[512];
                    snprintf(old_path, sizeof(old_path), "./html/content/posts/%s/%s", username, oldname);
                    snprintf(new_path, sizeof(new_path), "./html/content/posts/%s/%s", username, newname);

                    if (rename(old_path, new_path) == 0 || errno == ENOENT) {
                        cms_rename_media_directory(dir_id, newname);

                        CmsMediaDirectory dir;
                        if (cms_get_media_directory_by_id(dir_id, &dir)) {
                            strncpy(dir.author_name, username, sizeof(dir.author_name) - 1);
                            char *html = media_admin_render_directory_item(&dir, epoch);
                            if (html) {
                                char hdr[128];
                                snprintf(hdr, sizeof(hdr),
                                         "Content-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\n",
                                         strlen(html));
                                char resp[256];
                                snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\n%s\r\n", hdr);
                                connection_write(ctx, resp, strlen(resp));
                                connection_write(ctx, html, strlen(html));
                                free(html);
                            }
                        }
                    } else {
                        send_simple(ctx, "500 Internal Server Error", "Could not rename directory");
                    }
                    connection_close(ctx);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/directory/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    char dir_id[32], dirname[256];
                    parse_urlencoded_field(req.body, req.body_length, "id", dir_id, sizeof(dir_id));
                    parse_urlencoded_field(req.body, req.body_length, "dirname", dirname, sizeof(dirname));

                    char username[64] = {0};
                    cms_get_username_by_id(user_id, username, sizeof(username));

                    char dirpath[512];
                    snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, dirname);

                    rmdir(dirpath);
                    cms_delete_media_directory(dir_id);
                    send_simple(ctx, "200 OK", "Deleted");
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/media/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_dashboard_session(ctx, &req, epoch, user_id)) {
                    const char *ct = get_header_value(&req, "Content-Type");
                    MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                    if (!mp) {
                        send_simple(ctx, "400 Bad Request", "Invalid multipart body");
                    } else {
                        const MultipartPart *dir_part = multipart_find(mp, "media-directory-selected");
                        const MultipartPart *file_part = multipart_find(mp, "file");

                        if (!dir_part || !file_part || file_part->filename[0] == '\0') {
                            send_simple(ctx, "400 Bad Request", "Missing directory or file");
                            free_multipart(mp);
                        } else {
                            char dir_id_buf[32] = {0};
                            size_t dlen = dir_part->data_len < sizeof(dir_id_buf) - 1 ? dir_part->data_len : sizeof(dir_id_buf) - 1;
                            memcpy(dir_id_buf, dir_part->data, dlen);

                            CmsMediaDirectory dir;
                            if (!cms_get_media_directory_by_id(dir_id_buf, &dir)) {
                                send_simple(ctx, "404 Not Found", "Directory not found");
                            } else {
                                char username[64] = {0};
                                cms_get_username_by_id(user_id, username, sizeof(username));

                                // Sanitize filename
                                char sanitized[256] = {0};
                                int j = 0;
                                for (int k = 0; file_part->filename[k] && j < (int)sizeof(sanitized) - 1; k++) {
                                    char c = file_part->filename[k];
                                    if (c == ' ') sanitized[j++] = '-';
                                    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                             (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
                                        sanitized[j++] = c;
                                }
                                sanitized[j] = '\0';

                                // Create dir on disk
                                char dirpath[512];
                                snprintf(dirpath, sizeof(dirpath), "./html/content/posts/%s/%s", username, dir.name);
                                mkdir(dirpath, 0775);

                                // Write file
                                char filepath[1024];
                                snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, sanitized);
                                FILE *f = fopen(filepath, "wb");
                                int write_ok = 0;
                                if (f) {
                                    write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                                    fclose(f);
                                }

                                if (!write_ok) {
                                    send_simple(ctx, "500 Internal Server Error", "Could not write file");
                                } else {
                                    LOG_INFO("Media file saved: %s (%zu bytes)", filepath, file_part->data_len);

                                    // Run image optimizer
                                    char command[4096];
                                    snprintf(command, sizeof(command),
                                             "./scripts/image-optimizer.sh '%s' '%s' '%s'",
                                             dirpath, dirpath, filepath);
                                    LOG_INFO("Running optimizer: %s", command);

                                    char optimized_path[1024] = {0};
                                    FILE *pipe = popen(command, "r");
                                    if (pipe) {
                                        if (fgets(optimized_path, sizeof(optimized_path), pipe))
                                            optimized_path[strcspn(optimized_path, "\n")] = '\0';
                                        pclose(pipe);
                                    }

                                    if (optimized_path[0] == '\0')
                                        strncpy(optimized_path, filepath, sizeof(optimized_path) - 1);

                                    // Build filename for DB: strip path, remove _full suffix
                                    char *basename = strrchr(optimized_path, '/');
                                    basename = basename ? basename + 1 : optimized_path;
                                    char db_name[512];
                                    strncpy(db_name, basename, sizeof(db_name) - 1);
                                    db_name[sizeof(db_name) - 1] = '\0';
                                    char *dot = strrchr(db_name, '.');
                                    if (dot) {
                                        char *full_tag = strstr(db_name, "_full");
                                        if (full_tag && full_tag < dot)
                                            memmove(full_tag, full_tag + 5, strlen(full_tag + 5) + 1);
                                    }

                                    char media_id[25];
                                    cms_insert_media(db_name, user_id, dir_id_buf, media_id);

                                    char json_response[2048];
                                    snprintf(json_response, sizeof(json_response),
                                             "{\"ok\":true,\"filename\":\"%s\"}", optimized_path);
                                    char *response = build_json_response(json_response);
                                    connection_write(ctx, response, strlen(response));
                                    free(response);
                                }
                            }
                            free_multipart(mp);
                            connection_close(ctx);
                        }
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/settings/fonts/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *ct = get_header_value(&req, "Content-Type");
                    MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                    const MultipartPart *name_part = mp ? multipart_find(mp, "name") : NULL;
                    const MultipartPart *file_part = mp ? multipart_find(mp, "file") : NULL;

                    // Font names become both a Mongo document field and a
                    // literal CSS font-family value (see page_layout.c's
                    // build_logo_font_css()) - restricted to a plain-text
                    // whitelist so neither can break out of either context.
                    char name[81] = "";
                    if (name_part && name_part->data_len > 0) {
                        size_t len = name_part->data_len < sizeof(name) - 1
                            ? name_part->data_len : sizeof(name) - 1;
                        size_t j = 0;
                        for (size_t k = 0; k < len; k++) {
                            char c = name_part->data[k];
                            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')
                                name[j++] = c;
                        }
                        name[j] = '\0';
                    }

                    char sanitized[256];
                    int valid_ext = 0;
                    if (file_part && file_part->filename[0] &&
                        sanitize_asset_filename(file_part->filename, sanitized, sizeof(sanitized))) {
                        const char *dot = strrchr(sanitized, '.');
                        static const char *allowed[] = {".ttf", ".otf", ".woff", ".woff2"};
                        for (size_t i = 0; dot && i < sizeof(allowed) / sizeof(allowed[0]); i++)
                            if (strcasecmp(dot, allowed[i]) == 0) { valid_ext = 1; break; }
                    }

                    if (!name[0] || !valid_ext) {
                        char *response = build_redirect_response(
                            "/dashboard/settings/fonts?error=Name+and+a+.ttf%2F.otf%2F.woff%2F.woff2+file+are+required",
                            "", epoch);
                        send_or_error(ctx, response, req.method, epoch);
                    } else {
                        mkdir_recursive("./html/assets/fonts");

                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./html/assets/fonts/%s", sanitized);

                        FILE *f = fopen(filepath, "wb");
                        int write_ok = 0;
                        if (f) {
                            write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                            fclose(f);
                        }

                        if (!write_ok) {
                            char *response = build_redirect_response(
                                "/dashboard/settings/fonts?error=Could+not+write+font+file", "", epoch);
                            send_or_error(ctx, response, req.method, epoch);
                        } else {
                            cms_add_font(name, sanitized);
                            LOG_INFO("Font saved: %s (%s)", filepath, name);
                            char *response = build_redirect_response("/dashboard/settings/fonts", "", epoch);
                            send_or_error(ctx, response, req.method, epoch);
                        }
                    }

                    free_multipart(mp);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   match_id_route(decoded_url, "/dashboard/settings/fonts", "/delete", id, sizeof(id))) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                char *response = build_redirect_response("/dashboard", "", epoch);
                send_or_error(ctx, response, req.method, epoch);
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    char filename[256];
                    if (cms_get_font_filename(id, filename, sizeof(filename)) && filename[0]) {
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "./html/assets/fonts/%s", filename);
                        remove(filepath);
                    }
                    cms_delete_font(id);
                    char *response = build_redirect_response("/dashboard/settings/fonts", "", epoch);
                    send_or_error(ctx, response, req.method, epoch);
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/theme-assets/upload") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *theme_key = get_query_param(params, param_count, "theme");
                    const char *component = get_query_param(params, param_count, "component");
                    const char *epoch_str = get_query_param(params, param_count, "epoch");

                    char dir[256];
                    if (!theme_assets_dir(theme_key, component, epoch_str, dir, sizeof(dir))) {
                        send_simple(ctx, "400 Bad Request", "Invalid theme, component or epoch");
                    } else {
                        const char *ct = get_header_value(&req, "Content-Type");
                        MultipartResult *mp = parse_multipart(req.body, req.body_length, ct);

                        const MultipartPart *file_part = mp ? multipart_find(mp, "file") : NULL;
                        char sanitized[256];
                        if (!file_part || file_part->filename[0] == '\0' ||
                            !sanitize_asset_filename(file_part->filename, sanitized, sizeof(sanitized))) {
                            send_simple(ctx, "400 Bad Request", "Missing or invalid file");
                        } else {
                            mkdir_recursive(dir);

                            char filepath[512];
                            snprintf(filepath, sizeof(filepath), "%s/%s", dir, sanitized);

                            FILE *f = fopen(filepath, "wb");
                            int write_ok = 0;
                            if (f) {
                                write_ok = fwrite(file_part->data, 1, file_part->data_len, f) == file_part->data_len;
                                fclose(f);
                            }

                            if (!write_ok) {
                                send_simple(ctx, "500 Internal Server Error", "Could not write file");
                            } else {
                                LOG_INFO("Theme asset saved: %s (%zu bytes)", filepath, file_part->data_len);
                                char *escaped = json_escape_alloc(sanitized);
                                char *json = render_template("{\"ok\":true,\"filename\":\"%s\"}", escaped ? escaped : sanitized);
                                free(escaped);
                                char *response = build_json_response(json ? json : "{\"ok\":true}");
                                free(json);
                                connection_write(ctx, response, strlen(response));
                                free(response);
                            }
                        }

                        free_multipart(mp);
                        connection_close(ctx);
                    }
                }
            }

        } else if (strcmp(req.method, "POST") == 0 &&
                   strcmp(decoded_url, "/dashboard/api/theme-assets/delete") == 0) {
            int epoch = resolve_epoch(&req);
            if (epoch != EPOCH_MODERN) {
                send_simple(ctx, "403 Forbidden", "Forbidden");
            } else {
                char user_id[USER_ID_HEX_BUF_SIZE];
                if (require_admin_session(ctx, &req, epoch, user_id)) {
                    const char *theme_key = get_query_param(params, param_count, "theme");
                    const char *component = get_query_param(params, param_count, "component");
                    const char *epoch_str = get_query_param(params, param_count, "epoch");
                    const char *file = get_query_param(params, param_count, "file");

                    char dir[256];
                    char sanitized[256];
                    if (!theme_assets_dir(theme_key, component, epoch_str, dir, sizeof(dir)) ||
                        !sanitize_asset_filename(file, sanitized, sizeof(sanitized)) ||
                        strcmp(file, sanitized) != 0) {
                        send_simple(ctx, "400 Bad Request", "Invalid theme, component, epoch or file");
                    } else {
                        char filepath[512];
                        snprintf(filepath, sizeof(filepath), "%s/%s", dir, sanitized);
                        unlink(filepath);
                        send_simple(ctx, "200 OK", "Deleted");
                    }
                }
            }

        } else if (strcmp(req.method, "OPTIONS") == 0) {
            const char *opts =
                "HTTP/1.1 204 No Content\r\n"
                "Allow: GET, HEAD, OPTIONS, POST\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            connection_write(ctx, opts, strlen(opts));

        } else {
            send_error_response(ctx, 405, "405 Method Not Allowed", resolve_epoch(&req));
        }
    }

cleanup:
    free(raw_request);
    if (req.body) { free(req.body); req.body = NULL; }
    connection_close(ctx);
}
