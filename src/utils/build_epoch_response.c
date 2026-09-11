#include "build_epoch_response.h"
#include "detect_epoch.h"
#include "generate_url_theme.h"
#include "read_file.h"
#include "request_lang.h"
#include "request_theme.h"
#include "template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: SAMEORIGIN\r\n"

static const char *content_type_for_epoch(int epoch) {
    switch (epoch) {
        case EPOCH_WML:
            return "text/vnd.wap.wml";
        case EPOCH_EARLY:
            // No charset parameter: real Mosaic (2.1.1 confirmed) matches
            // Content-Type against its configured MIME viewers literally,
            // charset parameter and all, and "text/html; charset=iso-8859-1"
            // simply isn't a type it knows - "Undefined Viewer for MIME
            // Type" instead of the page. HTTP's own default charset is
            // ISO-8859-1 (RFC 1945/2616) if none is given, which is exactly
            // what the body now actually is (see utf8_to_latin1()), so
            // leaving the header bare still lands on the right charset.
            return "text/html";
        default:
            // Epoch 0's realistic reader is a terminal browser (Lynx/w3m/
            // ELinks) running today in a UTF-8 locale, not a genuinely
            // pre-Unicode machine like epoch 1's Mosaic - and that's the
            // same audience the QR blocks' Unicode half-blocks are drawn
            // for (see qr_generator.c), so this stays UTF-8 rather than
            // getting the epoch 1/WML Latin-1 treatment.
            return "text/html; charset=UTF-8";
    }
}

// Every stored/authored string in the CMS is UTF-8, but UTF-8 wasn't defined
// until 1993 and didn't reach the web until HTML 4 (1997) - epoch 0/1 (and
// WML) predate it entirely and assume ISO-8859-1, HTTP's own default charset
// (RFC 1945/2616). Handed raw UTF-8 bytes with no matching charset, they
// render each encoded character as two/three separate Latin-1 glyphs (the
// "SÃguenos" / "niÃ±o" mangling): the fix is to actually transcode the body
// to Latin-1 instead of just relabeling the header. Only the Latin-1 range
// (U+0000-U+00FF - ASCII plus the accented Western European letters the CMS
// content actually uses) survives; anything further out (a stray emoji,
// CJK, ...) has no Latin-1 byte to become, so it degrades to '?' rather than
// corrupting the byte stream.
static char *utf8_to_latin1(const char *utf8) {
    size_t len = strlen(utf8);
    char *out = malloc(len + 1);
    if (!out) return NULL;

    const unsigned char *p = (const unsigned char *)utf8;
    char *w = out;
    while (*p) {
        if (*p < 0x80) {
            *w++ = (char)*p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            unsigned int cp = ((unsigned int)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            *w++ = (cp <= 0xFF) ? (char)cp : '?';
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            *w++ = '?'; // U+0800-U+FFFF - always past the Latin-1 range
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            *w++ = '?'; // U+10000 and up
            p += 4;
        } else {
            *w++ = '?'; // not valid UTF-8 - skip the one bad byte and resync
            p += 1;
        }
    }
    *w = '\0';
    return out;
}

// Epoch 0/1 (and WML) have no way to remember a language choice across a
// click, and epoch 1/2 have no reliable way to remember a theme choice
// either (Max-Age-only cookies - see http_router.c's /language/set and
// /theme/set - are silently dropped or session-only on real HTTP/1.0-era
// clients, and epoch 1's redirect-based /set routes are skipped entirely in
// favor of a direct "?lang=xx"/"?theme=xx" link for the same reason - see
// language_page.c/theme_page.c). So instead of threading "?lang=xx"/
// "?theme=xx" through every module that ever builds a link - menu,
// categories, blog list, galleries, pagination... - every site-relative
// href in the finished page gets it stitched on right here, in the one
// place every one of those pages passes through before going out.
// "Site-relative" means it starts with a single '/' - "//host/..."
// (protocol-relative), "http(s)://...", "mailto:", "javascript:" and
// "#fragment" links are left alone. A link that already carries this param
// (e.g. epoch 1's own direct "?lang=xx"/"?theme=xx" switch links) is left
// untouched rather than double-tagged.
static char *inject_query_param_into_links(const char *html, const char *param, const char *value) {
    size_t param_len = strlen(param);
    size_t value_len = strlen(value);
    size_t extra_per_link = 5 + param_len + 1 + value_len; // worst case: "&amp;" + param + "=" + value

    size_t link_count = 0;
    for (const char *p = html; (p = strstr(p, "href=\"")) != NULL; p += 6) link_count++;
    if (link_count == 0) return strdup(html);

    char *out = malloc(strlen(html) + link_count * extra_per_link + 1);
    if (!out) return NULL;

    char tag[64];
    snprintf(tag, sizeof(tag), "%s=", param);

    char *w = out;
    const char *p = html;
    for (;;) {
        const char *marker = strstr(p, "href=\"");
        if (!marker) {
            strcpy(w, p);
            break;
        }

        size_t prefix_len = (size_t)(marker - p);
        memcpy(w, p, prefix_len);
        w += prefix_len;

        memcpy(w, "href=\"", 6);
        w += 6;

        const char *val_start = marker + 6;
        const char *val_end = strchr(val_start, '"');
        if (!val_end) {
            strcpy(w, val_start);
            w += strlen(val_start);
            break;
        }
        size_t val_len = (size_t)(val_end - val_start);

        memcpy(w, val_start, val_len);
        w += val_len;

        char valbuf[2048];
        size_t copy_len = val_len < sizeof(valbuf) - 1 ? val_len : sizeof(valbuf) - 1;
        memcpy(valbuf, val_start, copy_len);
        valbuf[copy_len] = '\0';

        int is_site_relative = val_len >= 1 && val_start[0] == '/' &&
                               (val_len == 1 || val_start[1] != '/');
        int already_tagged = strstr(valbuf, tag) != NULL;

        if (is_site_relative && !already_tagged) {
            // An href attribute is XML/HTML text content: a bare '&' is
            // fine to most HTML parsers but not well-formed - and WML is
            // strict XML, where it is a hard parse error ("element is not
            // well formed"), confirmed live in a WAP emulator.
            if (memchr(val_start, '?', val_len)) {
                memcpy(w, "&amp;", 5);
                w += 5;
            } else {
                *w++ = '?';
            }
            memcpy(w, param, param_len);
            w += param_len;
            *w++ = '=';
            memcpy(w, value, value_len);
            w += value_len;
        }

        *w++ = '"';
        p = val_end + 1;
    }

    return out;
}

// WML is strict XML: a literal, author-typed "<br>" - not run through
// expand_newlines(), which already emits the self-closing form for WML - is
// not well-formed and fails to parse in a real WAP client ("Syntax error 12
// - the element is not well formed", confirmed live in a WAP emulator).
static char *wml_close_br(const char *html) {
    size_t count = 0;
    for (const char *p = html; (p = strstr(p, "<br>")) != NULL; p += 4) count++;
    if (count == 0) return strdup(html);

    char *out = malloc(strlen(html) + count + 1); // "<br>" -> "<br/>" is +1 byte each
    if (!out) return NULL;

    char *w = out;
    const char *p = html;
    for (;;) {
        const char *m = strstr(p, "<br>");
        if (!m) { strcpy(w, p); break; }
        size_t n = (size_t)(m - p);
        memcpy(w, p, n);
        w += n;
        memcpy(w, "<br/>", 5);
        w += 5;
        p = m + 4;
    }
    return out;
}

// WML has no list element at all - <ul>/<ol>/<li> are simply not in its
// vocabulary. The CMS's own "list" content block already renders WML-safe
// output (see list-item_epoch-1.html: "- item<br/>", no tags), but an
// author can also type a raw HTML list directly into a paragraph's rich
// text, which does not go through that template at all - confirmed live,
// one such list in an existing article ("Syntax error 12", a WAP emulator
// rejecting <ul>/<li> outright). Converts to the same "- item<br/>"
// convention the dedicated block already uses.
static char *wml_strip_lists(const char *html) {
    // <ul>/</ul>/<ol>/</ol> are dropped (shrinks); <li> (4 bytes) becomes
    // "- " (2 bytes, shrinks); </li> (5 bytes) becomes "<br/>" (5 bytes,
    // same) - so the output never grows past the input length.
    char *out = malloc(strlen(html) + 1);
    if (!out) return NULL;

    char *w = out;
    const char *p = html;
    while (*p) {
        if      (strncasecmp(p, "<ul>",   4) == 0) { p += 4; }
        else if (strncasecmp(p, "</ul>",  5) == 0) { p += 5; }
        else if (strncasecmp(p, "<ol>",   4) == 0) { p += 4; }
        else if (strncasecmp(p, "</ol>",  5) == 0) { p += 5; }
        else if (strncasecmp(p, "<li>",   4) == 0) { memcpy(w, "- ", 2); w += 2; p += 4; }
        else if (strncasecmp(p, "</li>",  5) == 0) { memcpy(w, "<br/>", 5); w += 5; p += 5; }
        else { *w++ = *p++; }
    }
    *w = '\0';
    return out;
}

// Epoch 0/1/WML/2 fixups in one place. The language-preserving link rewrite
// applies to epoch 0/1/WML - none of them can carry a cookie through a
// redirect (see request_lang.h). The theme-preserving rewrite applies to
// epoch 1/2 - both have the same Max-Age-cookie problem for a theme choice
// (see request_theme.h/http_router.c's /theme/set), epoch 0/WML skip it
// simply because neither offers a theme control at all (see menu.c's
// theme_selector()). The Latin-1 transcode is narrower still: only epoch 1
// (Mosaic-era GUI browsers) and WML actually predate UTF-8; epoch 0's real
// audience is a modern terminal browser in a UTF-8 locale; see
// content_type_for_epoch() and qr_generator.c's ASCII-vs-half-block QR
// choice for the same reasoning. WML alone also gets its <br> tags closed
// and any raw HTML list converted, since both are hard WML validity errors.
// Returns a fresh allocation, or NULL only when nothing needed changing
// (the caller keeps using its original body).
static char *retrofit_body_for_epoch(const char *body, int epoch) {
    if (epoch > EPOCH_MIDDLE) return NULL;

    char *step = NULL;
    const char *cur = body;

    if (epoch <= EPOCH_EARLY) {
        char *tagged = inject_query_param_into_links(cur, "lang", request_lang());
        if (tagged) { free(step); step = tagged; cur = step; }
    }

    if (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE) {
        char *tagged = inject_query_param_into_links(cur, "theme", request_theme());
        if (tagged) { free(step); step = tagged; cur = step; }
    }

    if (epoch == EPOCH_WML) {
        char *closed = wml_close_br(cur);
        if (closed) { free(step); step = closed; cur = step; }

        char *listless = wml_strip_lists(cur);
        if (listless) { free(step); step = listless; cur = step; }
    }

    if (epoch != EPOCH_EARLY && epoch != EPOCH_WML)
        return step;

    char *latin1 = utf8_to_latin1(cur);
    free(step);
    return latin1;
}

char *build_epoch_response(const char *body, const char *extra_headers, int epoch) {
    return build_epoch_response_status(body, extra_headers, epoch, "200 OK");
}

char *build_epoch_response_status(const char *body, const char *extra_headers,
                                   int epoch, const char *status_line) {
    const char *content_type = content_type_for_epoch(epoch);

    char *rewritten = retrofit_body_for_epoch(body, epoch);
    if (rewritten) body = rewritten;

    size_t body_len = strlen(body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, content_type, body_len, extra_headers);
    if (header_len < 0) {
        free(rewritten);
        return NULL;
    }

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) {
        free(rewritten);
        return NULL;
    }

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, content_type, body_len, extra_headers);

    memcpy(response + header_len, body, body_len + 1);
    free(rewritten);

    return response;
}

// Tiny epoch-appropriate "click here to continue" body for clients that
// don't auto-follow a 302's Location header. Loaded from
// html/themes/<theme>/redirect/redirect_epoch<N>.html.
static char *build_redirect_body(const char *location, int epoch) {
    char *path = generate_url_theme("redirect/redirect_epoch%d.html", epoch);
    if (!path) return NULL;

    char *tpl = read_file_to_string(path);
    free(path);
    if (!tpl) return NULL;

    char *body = render_template(tpl, location, location);
    free(tpl);
    return body;
}

char *build_redirect_response(const char *location, const char *extra_headers, int epoch) {
    char *body = build_redirect_body(location, epoch);
    if (!body) return NULL;

    char *rewritten = retrofit_body_for_epoch(body, epoch);
    if (rewritten) {
        free(body);
        body = rewritten;
    }

    const char *content_type = content_type_for_epoch(epoch);
    size_t body_len = strlen(body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        location, content_type, body_len, extra_headers);
    if (header_len < 0) {
        free(body);
        return NULL;
    }

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) {
        free(body);
        return NULL;
    }

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        location, content_type, body_len, extra_headers);

    memcpy(response + header_len, body, body_len + 1);
    free(body);

    return response;
}

char *build_json_response_status(const char *json_body, const char *status_line) {
    size_t body_len = strlen(json_body);

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n"
        "Content-Length: %zu\r\n"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, body_len);
    if (header_len < 0) return NULL;

    char *response = malloc((size_t)header_len + body_len + 1);
    if (!response) return NULL;

    snprintf(response, (size_t)header_len + 1,
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n"
        "Content-Length: %zu\r\n"
        SECURITY_HEADERS
        "Connection: close\r\n"
        "\r\n",
        status_line, body_len);

    memcpy(response + header_len, json_body, body_len + 1);

    return response;
}

char *build_json_response(const char *json_body) {
    return build_json_response_status(json_body, "200 OK");
}
