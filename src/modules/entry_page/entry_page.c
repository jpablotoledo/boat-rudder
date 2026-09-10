#include "entry_page.h"
#include "../../db/cms_themes.h"
#include "../../utils/category_tags.h"
#include "../../utils/image_size.h"
#include "../../utils/detect_epoch.h"
#include "../../utils/generate_url_theme.h"
#include "../../utils/qr_generator/qr_generator.h"
#include "../../utils/read_file.h"
#include "../../utils/request_theme.h"
#include "../../utils/template_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Widest an image may be drawn on the retro epochs, in pixels.
//
// Their browsers ran on 640x480 screens, and there is no reflow to rescue a
// layout that overshoots: the page simply grows a horizontal scrollbar and the
// text runs off the right edge. The budget at 640 is roughly 620 usable once
// the vertical scrollbar is taken out, 95% of that for the content table, less
// its 10px cellpadding on each side and the image's own 5px hspace - about
// 560. Capping a little under that keeps a full-width image inside the column.
#define RETRO_MAX_IMAGE_WIDTH 550

// Where generated QR assets are written. Matches generate_url_theme(), which
// likewise resolves templates against "./html" from the process working dir.
#define HTML_ROOT_DIR "./html"

static char *load_template(const char *subpath_fmt, int epoch) {
    char *path = generate_url_theme(subpath_fmt, epoch);
    char *tpl  = path ? read_file_to_string(path) : NULL;
    free(path);
    return tpl;
}

// On presentational blocks extra_data names a style variant, never raw markup.
// The previous CMS stored whole attributes there (style="font-size: 18px") and
// interpolated them unescaped, so anyone with editor access could inject
// arbitrary HTML into a page. Here only [a-z0-9-] passes and the renderer
// builds the class itself; anything else yields no modifier at all.
//
// Writes a leading-space class suffix ("" when there is no valid modifier) so
// templates can append it directly to their base class.
static void modifier_class(const char *extra_data, const char *base,
                           char *out, size_t out_size) {
    out[0] = '\0';
    if (!extra_data || !extra_data[0]) return;

    for (const char *p = extra_data; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) return;
    }
    snprintf(out, out_size, " %s--%s", base, extra_data);
}

// extra_data holds the heading level ("1".."6"). Anything else - including the
// empty value most blocks were migrated with - falls back to the level this
// renderer used to hardcode, so untagged headings keep looking the same.
static const char *heading_level(const char *extra_data) {
    if (extra_data && extra_data[0] >= '1' && extra_data[0] <= '6' && !extra_data[1])
        return extra_data;
    return "2";
}

static char *render_title(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/title/title_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // WML has no headings; its template renders bold text and takes the text alone.
    char *result;
    if (epoch == EPOCH_WML) {
        result = render_template(tpl, block->text);
    } else if (epoch == EPOCH_EARLY) {
        // No stylesheet here, so - like render_paragraph()'s epoch 1/2 case -
        // the title's color has to be a real attribute rather than a CSS
        // class nothing defines. Was hardcoded to white, unreadable on
        // light-background themes; now takes the theme's own
        // home-content-text color, the same "Home content" setting
        // render_paragraph() reads.
        const char *level = heading_level(block->extra_data);
        CmsThemeColors colors;
        cms_get_theme_colors(request_theme(), &colors);
        result = render_template(tpl, level, colors.home_content_text, block->text, level);
    } else {
        const char *level = heading_level(block->extra_data);
        result = render_template(tpl, level, block->text, level);
    }
    free(tpl);
    return result;
}

// Paragraph text is stored either as HTML (what the dashboard editor writes)
// or as plain text with literal newlines (what the migration produced). HTML
// collapses a bare newline into a space, so a stored "\n" would silently
// disappear; expand it into a line break the target epoch understands.
// Returns a malloc'd copy, or NULL on allocation failure.
static char *expand_newlines(const char *text, int epoch) {
    if (!text) return strdup("");

    // WML is XHTML-strict and needs the self-closing form.
    const char *br = (epoch == EPOCH_WML) ? "<br/>" : "<br>";
    size_t br_len = strlen(br);

    size_t breaks = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') breaks++;
    if (breaks == 0) return strdup(text);

    char *out = malloc(strlen(text) + breaks * br_len + 1);
    if (!out) return NULL;

    char *w = out;
    for (const char *p = text; *p; p++) {
        if (*p == '\r') continue;          // normalise CRLF to a single break
        if (*p == '\n') {
            memcpy(w, br, br_len);
            w += br_len;
        } else {
            *w++ = *p;
        }
    }
    *w = '\0';
    return out;
}

// Two things a rich-text paragraph can contain that epochs 0-1 cannot be
// trusted to render, however they got into the stored text - typed directly
// by an author, or produced by expand_newlines() from a blank line in
// plain-text content:
//
//   - A bare "&nbsp;" (an author indenting a line, or forcing a run of
//     spaces to survive HTML's whitespace collapsing) prints as the literal
//     six characters &nbsp; instead of a space: named entity sets such as
//     ISOnum/HTMLlat1 only entered the language via the HTML 2.0 DTD
//     (RFC 1866, 1995), and epoch 1 reaches back to NCSA Mosaic 1.x and
//     Cello (1993), both of which predate it.
//   - A run of 2+ consecutive <br> - a paragraph break - does not reliably
//     leave a visible gap: an engine of that era can collapse a run of
//     otherwise-empty lines, the same way it collapses runs of source
//     whitespace, since nothing sits on the blank line to give it a height
//     of its own.
//
// Both are fixed the same way: replace the named reference with the
// *numeric* one, "&#160;" - a base SGML mechanism, not a DTD-declared entity
// set, and so older and more consistently implemented than any named form.
// Splicing one into the middle of a <br> run puts real content on that
// line, so it cannot be collapsed away either.
static char *retro_safe_paragraph_text(const char *text, int epoch) {
    if (epoch != EPOCH_PRESTANDARD && epoch != EPOCH_EARLY) return strdup(text);

    static const char BR[]   = "<br>";
    static const size_t BR_LEN = sizeof(BR) - 1;
    static const char NBSP[] = "&nbsp;";
    static const size_t NBSP_LEN = sizeof(NBSP) - 1;

    char *out = strdup("");
    const char *p = text;
    while (out && *p) {
        if (strncmp(p, NBSP, NBSP_LEN) == 0) {
            out = str_append(out, "&#160;");
            p += NBSP_LEN;
            continue;
        }

        if (strncmp(p, BR, BR_LEN) != 0) {
            char one[2] = { *p, '\0' };
            out = str_append(out, one);
            p++;
            continue;
        }

        int run = 0;
        const char *q = p;
        while (strncmp(q, BR, BR_LEN) == 0) { run++; q += BR_LEN; }

        if (run >= 2) {
            char gap[32];
            snprintf(gap, sizeof(gap), "%s&#160;%s", BR, BR);
            out = str_append(out, gap);
        } else {
            out = str_append(out, BR);
        }
        p = q;
    }
    return out;
}

static char *render_paragraph(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/paragraph/paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char *expanded = expand_newlines(block->text, epoch);
    char *text = expanded ? retro_safe_paragraph_text(expanded, epoch) : NULL;
    free(expanded);
    char *result;
    if (!text) {
        result = NULL;
    } else if (epoch <= EPOCH_PRESTANDARD) {
        // Text-only and WML templates carry no classes, so they take the text alone.
        result = render_template(tpl, text);
    } else if (epoch < EPOCH_MODERN) {
        // No stylesheet here, so the "note" variant has to be a real colour
        // attribute rather than a CSS class nothing defines - the class alone
        // was a silent no-op on these epochs. A plain paragraph takes the
        // theme's own home-content-text color instead of a hardcoded white,
        // which read as unreadable on light-background themes.
        CmsThemeColors colors;
        const char *color;
        if (block->extra_data && strcmp(block->extra_data, "note") == 0) {
            color = "#deb887";
        } else {
            cms_get_theme_colors(request_theme(), &colors);
            color = colors.home_content_text;
        }
        result = render_template(tpl, color, text);
    } else {
        char mod[64];
        modifier_class(block->extra_data, "boat-rudder__paragraph", mod, sizeof(mod));
        result = render_template(tpl, mod, text);
    }
    free(text);
    free(tpl);
    return result;
}

static char *render_byline(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/byline/byline_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text, block->extra_data);
    free(tpl);
    return result;
}

// Blocks store the bare image path; the size variant is chosen here, so older
// browsers get a lighter file instead of the full-size one. Mirrors what
// render_gallery() already does, including epoch 1's GIF (those clients predate
// progressive JPEG). Epochs <= 0 render no image at all.
static char *image_for_epoch(const char *url, int epoch) {
    if (!url || !url[0]) return strdup("");

    const char *suffix = (epoch >= EPOCH_MODERN) ? "_full" : "_medium";
    char *variant = image_url_variant(url, suffix);

    // image-optimizer.sh writes _medium and _micro as GIF (only _full/_half/
    // _small stay JPEG), so the extension has to follow the variant or the
    // file is a 404.
    if (variant && epoch < EPOCH_MODERN) {
        char *dot = strrchr(variant, '.');
        if (dot) strcpy(dot, ".gif");
    }
    return variant;
}

// An image block's extra_data packs three fields as "caption|width|align",
// following the same convention as social-networks ("icon|url"). Width and
// align are validated against fixed sets, so a hand-edited value can never
// reach the page as markup. A value with no '|' is all caption, which is how
// every block written before this option looked.
typedef struct {
    char caption[512];
    const char *width; // "100" | "50" | "30"
    const char *align; // "left" | "center" | "right"
} ImageOptions;

static ImageOptions parse_image_options(const char *extra_data) {
    ImageOptions o = { .caption = "", .width = "100", .align = "center" };
    if (!extra_data || !extra_data[0]) return o;

    const char *first = strchr(extra_data, '|');
    size_t caption_len = first ? (size_t)(first - extra_data) : strlen(extra_data);
    if (caption_len >= sizeof(o.caption)) caption_len = sizeof(o.caption) - 1;
    memcpy(o.caption, extra_data, caption_len);
    o.caption[caption_len] = '\0';
    if (!first) return o;

    char rest[64] = "";
    strncpy(rest, first + 1, sizeof(rest) - 1);
    char *second = strchr(rest, '|');
    if (second) *second++ = '\0';

    static const char *widths[] = { "100", "50", "30", NULL };
    for (int i = 0; widths[i]; i++)
        if (strcmp(rest, widths[i]) == 0) o.width = widths[i];

    static const char *aligns[] = { "left", "center", "right", NULL };
    for (int i = 0; second && aligns[i]; i++)
        if (strcmp(second, aligns[i]) == 0) o.align = aligns[i];

    return o;
}

static char *render_image(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/image/image_epoch%d.html", epoch);
    if (!tpl) return NULL;

    ImageOptions o = parse_image_options(block->extra_data);

    char *result;
    if (epoch <= EPOCH_PRESTANDARD) {
        // Text-only and WML render the caption alone, with no image at all.
        result = render_template(tpl, o.caption);
        free(tpl);
        return result;
    }

    char *src = image_for_epoch(block->text, epoch);
    // Clicking the image opens a bigger copy: epoch 3 hands it to the same
    // lightbox the galleries use, older epochs just link to the file, since
    // that viewer needs a media_galleries id an image block has no room for.
    // They get `_half` (capped at 1024px) rather than the original: a machine
    // of that era has neither the memory nor the link for a full-resolution
    // photo, and 1024 already exceeds anything it can display.
    const char *open_suffix = (epoch >= EPOCH_MODERN) ? "_full" : "_half";
    char *full = image_url_variant(block->text, open_suffix);
    if (!src || !full) {
        free(src);
        free(full);
        free(tpl);
        return NULL;
    }

    if (epoch < EPOCH_MODERN) {
        // Netscape 4 and IE 5 apply stylesheets only in part - `margin: auto`
        // on a block is exactly the sort of thing they get wrong - so size and
        // alignment travel as HTML attributes instead: align on the cell,
        // width on the image. Epoch 1 lays it out with <p align>, epoch 2 with
        // a table.
        //
        // The width has to be in pixels. Percentages on <img width> came with
        // HTML 4.0, and a browser of this era meeting one draws the image zero
        // pixels wide - it loads, it just cannot be seen. So the author's
        // 100/50/30 is resolved against the real width of the file being
        // served. If that width cannot be read the attribute is dropped and
        // the image comes out at its natural size, which beats not at all.
        char width_attr[32] = "";
        int intrinsic = image_intrinsic_width(src);
        int percent = atoi(o.width);
        if (intrinsic > 0 && percent > 0) {
            int px = intrinsic * percent / 100;
            if (px > RETRO_MAX_IMAGE_WIDTH) px = RETRO_MAX_IMAGE_WIDTH;
            if (px < 1) px = 1;
            // Only the width is emitted, never the height, so the browser
            // scales the other axis and the picture keeps its proportions.
            snprintf(width_attr, sizeof(width_attr), "width=\"%d\"", px);
        }
        result = render_template(tpl, o.align, full, src, width_attr, o.caption, o.caption);
    } else {
        char mods[128];
        snprintf(mods, sizeof(mods), " boat-rudder__entry-image--w%s boat-rudder__entry-image--%s",
                 o.width, o.align);
        // The full-size URL rides along in data-full for the lightbox.
        result = render_template(tpl, mods, src, full, o.caption, o.caption);
    }

    free(src);
    free(full);
    free(tpl);
    return result;
}

// Renders entry->category_names as a "tags" block. Returns "" (no block) if
// the entry has no categories.
static char *render_categories(const CmsEntry *entry, int epoch) {
    if (entry->category_count == 0) return strdup("");

    char *wrap_tpl = load_template("entry/entry-categories_epoch%d.html", epoch);
    if (!wrap_tpl) return strdup("");

    char *items = category_tags_render(entry->category_links, entry->category_names,
                                        entry->category_count, epoch);
    char *result = items ? render_template(wrap_tpl, items) : NULL;

    free(items);
    free(wrap_tpl);
    return result;
}

// One shape for every epoch: a container holding rows, each row holding items.
// Epochs that have no notion of a row - WML, text, and the epoch-3 grid - use a
// row template that is just its contents, so the loop below never branches.
// `per_row` is how many items a row holds before it is closed; 0 means one row.
static int gallery_columns(int epoch) {
    return (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE) ? 3 : 0;
}

// Thumbnail variant for the inline gallery. Epoch 1 predates progressive JPEG,
// so it gets the GIF the optimizer writes.
static char *gallery_thumb(const char *url, int epoch) {
    if (epoch == EPOCH_EARLY) {
        char *t = image_url_variant(url, "_micro");
        if (t) { char *dot = strrchr(t, '.'); if (dot) strcpy(dot, ".gif"); }
        return t;
    }
    return image_url_variant(url, "_small");
}

static char *render_gallery(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    const char *gallery_id = (block->extra_data && strlen(block->extra_data) == 24)
                              ? block->extra_data : NULL;

    // Text-only browsers (epoch 0) and WAP phones (WML) can't show any of
    // the photos - not even gallery_thumb()'s smallest variant renders as
    // anything but broken alt text on them - so instead of an "[img]" per
    // photo, the whole block collapses to one link into the gallery's own
    // page. That page (see the /gallery/<id> route) is itself a QR code the
    // reader can scan with a phone, since that machine cannot display the
    // pictures no matter which page it lands on. Mirrors the previous site.
    if (epoch <= EPOCH_PRESTANDARD) {
        if (!gallery_id) return strdup("");
        char *tpl = load_template("elements/gallery/gallery-view-link_epoch%d.html", epoch);
        char *result = tpl ? render_template(tpl, gallery_id) : NULL;
        free(tpl);
        return result ? result : strdup("");
    }

    char *wrap_tpl   = load_template("elements/gallery/gallery_epoch%d.html", epoch);
    char *row_tpl    = load_template("elements/gallery/gallery-row_epoch%d.html", epoch);
    char *item_tpl   = load_template("elements/gallery/gallery-item_epoch%d.html", epoch);
    char *more_tpl   = load_template("elements/gallery/gallery-item-more_epoch%d.html", epoch);
    char *hidden_tpl = load_template("elements/gallery/gallery-item-hidden_epoch%d.html", epoch);
    if (!wrap_tpl || !row_tpl || !item_tpl) {
        free(wrap_tpl); free(row_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
        return strdup("");
    }

    // Split the semicolon-separated list of image paths.
    int total = 1;
    for (const char *p = block->text; *p; p++) if (*p == ';') total++;
    char **urls = calloc((size_t)total, sizeof(char *));
    char *copy = strdup(block->text);
    int count = 0;
    if (urls && copy) {
        char *saveptr = NULL;
        for (char *tok = strtok_r(copy, ";", &saveptr); tok && count < total;
             tok = strtok_r(NULL, ";", &saveptr)) {
            while (*tok == ' ') tok++;
            if (*tok) urls[count++] = strdup(tok);
        }
    }
    free(copy);

    // Epoch 3 shows the first few and folds the rest behind a "+N" tile.
    // Epochs 1-2 cap harder and cut the rest entirely, with a "View all"
    // link/note after the gallery instead of a tile: a machine of that era is
    // the one actually paying for every image in the block, not just the
    // reader's patience, and an article can carry a gallery of 20+ photos.
    int cap = (epoch >= EPOCH_MODERN) ? 5
            : (epoch == EPOCH_EARLY || epoch == EPOCH_MIDDLE) ? 3 : 0;
    int max_visible = (cap > 0 && count > cap) ? cap : count;
    int remaining   = count - max_visible;

    int per_row = gallery_columns(epoch);
    char *rows_html = strdup("");
    char *row_html  = strdup("");
    int col = 0;

    // Epochs 1-2 simply never render the images past the cap - not hidden
    // markup, nothing sent for them at all. Epoch 3's overflow tile is the
    // only case that renders every item and hides the rest with CSS.
    int render_count = (epoch >= EPOCH_MODERN) ? count : max_visible;

    for (int i = 0; i < render_count && rows_html && row_html; i++) {
        char *thumb = gallery_thumb(urls[i], epoch);
        char *full  = image_url_variant(urls[i], "_full");

        // Where the block names a gallery the item opens the viewer at this
        // image; otherwise it opens the picture itself, so the item is never
        // a dead end and no separate "no link" template is needed.
        char *href = gallery_id ? render_template("/gallery/%s?img=%d", gallery_id, i)
                                : (full ? strdup(full) : strdup(""));

        const char *tpl = item_tpl;
        char *item;
        if (epoch >= EPOCH_MODERN && remaining > 0 && i == max_visible - 1 && more_tpl)
            item = render_template(more_tpl, thumb ? thumb : "", full ? full : "", remaining + 1);
        else if (epoch >= EPOCH_MODERN && i >= max_visible && hidden_tpl)
            item = render_template(hidden_tpl, thumb ? thumb : "", full ? full : "");
        else
            item = render_template(tpl, href ? href : "", thumb ? thumb : "", full ? full : "");

        free(thumb); free(full); free(href);
        if (item) { row_html = str_append(row_html, item); free(item); }

        if (per_row > 0 && ++col >= per_row) {
            char *row = render_template(row_tpl, row_html);
            if (row) { rows_html = str_append(rows_html, row); free(row); }
            free(row_html); row_html = strdup(""); col = 0;
        }
    }

    // The last row, or the only one when the epoch does not use rows.
    if (rows_html && row_html && (col > 0 || per_row == 0)) {
        char *row = render_template(row_tpl, row_html);
        if (row) { rows_html = str_append(rows_html, row); free(row); }
    }
    free(row_html);

    for (int i = 0; i < count; i++) free(urls[i]);
    free(urls);

    char *result = rows_html ? render_template(wrap_tpl, rows_html) : NULL;
    free(rows_html);

    // "View all" goes after the closing table, not inside a row - it names
    // how many photos were left out, and where to see them.
    if (result && cap > 0 && count > cap) {
        char *view_all = NULL;
        if (gallery_id) {
            char *tpl = load_template("elements/gallery/gallery-view-all_epoch%d.html", epoch);
            if (tpl) view_all = render_template(tpl, gallery_id, count);
            free(tpl);
        } else {
            char *tpl = load_template("elements/gallery/gallery-view-all-count_epoch%d.html", epoch);
            if (tpl) view_all = render_template(tpl, count);
            free(tpl);
        }
        if (view_all) { result = str_append(result, view_all); free(view_all); }
    }

    free(wrap_tpl); free(row_tpl); free(item_tpl); free(more_tpl); free(hidden_tpl);
    return result ? result : strdup("");
}

static char *render_separator(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/separator/separator_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // Epochs -1/0 emit a bare <br/>/<hr> with no class to modify.
    char *result;
    if (epoch <= EPOCH_PRESTANDARD) {
        result = render_template(tpl);
    } else {
        char mod[64];
        modifier_class(block->extra_data, "boat-rudder__separator", mod, sizeof(mod));
        result = render_template(tpl, mod);
    }
    free(tpl);
    return result;
}

static char *render_link(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/link/link_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl,
        block->extra_data ? block->extra_data : "#",
        block->text       ? block->text       : "");
    free(tpl);
    return result;
}

static char *render_list(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    // One container template for both kinds; the tag itself is an argument,
    // and it comes last so that WML - which has no list element and prints the
    // items in a plain <p> - simply leaves it unused rather than skipping a
    // position, which printf does not define.
    const char *tag = (block->extra_data && strcmp(block->extra_data, "ol") == 0)
        ? "ol" : "ul";

    char *container_tpl = load_template("elements/list/list-container_epoch%d.html", epoch);
    char *item_tpl      = load_template("elements/list/list-item_epoch%d.html", epoch);
    if (!container_tpl || !item_tpl) {
        free(container_tpl); free(item_tpl);
        return strdup("");
    }

    char *items = strdup("");
    char *copy  = strdup(block->text);
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, "\n", &saveptr); tok; tok = strtok_r(NULL, "\n", &saveptr)) {
        while (*tok == '\r') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end >= tok && (*end == '\r' || *end == '\n')) *end-- = '\0';
        if (!*tok) continue;
        char *item = render_template(item_tpl, tok);
        items = item ? str_append(items, item) : NULL;
        free(item);
    }
    free(copy);

    char *result = items ? render_template(container_tpl, items, tag) : NULL;
    free(items);
    free(container_tpl);
    free(item_tpl);
    return result ? result : strdup("");
}

static char *youtube_to_embed(const char *url) {
    if (!url || !url[0]) return NULL;
    if (strstr(url, "youtube.com/embed/")) return strdup(url);
    const char *v = strstr(url, "v=");
    if (v) {
        v += 2;
        char id[12] = {0};
        for (int i = 0; i < 11 && v[i] && v[i] != '&'; i++) id[i] = v[i];
        if (id[0]) {
            char *e = malloc(64);
            if (e) snprintf(e, 64, "https://www.youtube.com/embed/%s", id);
            return e;
        }
    }
    const char *ytbe = strstr(url, "youtu.be/");
    if (ytbe) {
        ytbe += 9;
        char id[12] = {0};
        for (int i = 0; i < 11 && ytbe[i] && ytbe[i] != '?' && ytbe[i] != '&'; i++) id[i] = ytbe[i];
        if (id[0]) {
            char *e = malloc(64);
            if (e) snprintf(e, 64, "https://www.youtube.com/embed/%s", id);
            return e;
        }
    }
    return strdup(url);
}

// Epoch 3 embeds an iframe. No earlier browser can play the video inline, so
// retro epochs render a QR code the reader scans with a phone, plus a text
// link - the QR asset differs per epoch (GIF / WBMP / plain-ASCII text) but
// all three templates take the same (asset, short_url, label) arguments.
static char *render_youtube_embed(const CmsContentBlock *block, int epoch) {
    if (!block->extra_data || !block->extra_data[0]) return strdup("");
    char *tpl = load_template("elements/youtube-embed/youtube-embed_epoch%d.html", epoch);
    if (!tpl) return NULL;

    if (epoch >= EPOCH_MODERN) {
        char *embed = youtube_to_embed(block->extra_data);
        char *result = embed ? render_template(tpl, embed) : NULL;
        free(embed);
        free(tpl);
        return result ? result : strdup("");
    }

    char video_id[32] = {0};
    if (extract_youtube_id(block->extra_data, video_id, sizeof(video_id)) != 0) {
        free(tpl);
        return strdup("");
    }

    char short_url[128];
    youtube_short_url(video_id, short_url, sizeof(short_url));
    const char *label = (block->text && block->text[0]) ? block->text : short_url;

    char *result = NULL;
    if (epoch == EPOCH_WML) {
        char wbmp_path[256];
        if (generate_youtube_qr_wbmp(block->extra_data, HTML_ROOT_DIR) == 0) {
            youtube_qr_wbmp_web_path(video_id, wbmp_path, sizeof(wbmp_path));
            result = render_template(tpl, wbmp_path, short_url, label);
        }
    } else if (epoch == EPOCH_PRESTANDARD) {
        // Text-only browsers get the QR drawn with Unicode half-blocks -
        // epoch 0 stays UTF-8 (see content_type_for_epoch()), unlike epoch
        // 1/WML, so this renders correctly for its realistic reader: a
        // terminal browser in a UTF-8 locale.
        char *qr_text = generate_qr_halfblock_text(short_url);
        if (qr_text) {
            result = render_template(tpl, qr_text, short_url, label);
            free(qr_text);
        }
    } else {
        char qr_path[256];
        if (generate_youtube_qr(block->extra_data, HTML_ROOT_DIR) == 0) {
            youtube_qr_web_path(video_id, qr_path, sizeof(qr_path));
            result = render_template(tpl, qr_path, short_url, label);
        }
    }

    free(tpl);
    return result ? result : strdup("");
}

static char *render_code_text(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/code-text/code-text_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // <pre> is only a reliable way to keep line breaks on browsers modern
    // enough to honor its whitespace-preservation rules. WML has no <pre> at
    // all, and NCSA Mosaic 1.x/Cello-era engines (epoch 0-1) don't honor it
    // consistently either - some builds collapse every \n inside <pre> the
    // same way they'd collapse whitespace in ordinary flow text, running the
    // whole block onto one line. Converting newlines to explicit <br> makes
    // the line breaks a browser command rather than whitespace it can choose
    // to ignore, so it degrades the same way on every epoch that keeps <pre>
    // in its template.
    char *text = (epoch <= EPOCH_EARLY) ? expand_newlines(block->text, epoch)
                                        : strdup(block->text);
    char *result = text ? render_template(tpl,
        block->extra_data ? block->extra_data : "", text) : NULL;
    free(text);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_generic(const CmsContentBlock *block, int epoch) {
    char *tpl = load_template("elements/generic/generic_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char *result = render_template(tpl, block->text ? block->text : "");
    free(tpl);
    return result ? result : strdup("");
}

// Unlike `image`/`gallery`, an image-paragraph block stores its path with the
// `_full` size suffix already baked in, not the bare path the renderer usually
// appends one to. Epochs 1-2 need the much lighter `_micro` instead, so the
// existing suffix has to be swapped out, not stacked under another one.
static char *image_paragraph_src(const char *stored_path, int epoch) {
    if (epoch != EPOCH_EARLY && epoch != EPOCH_MIDDLE) return strdup(stored_path);

    char *base = strdup(stored_path);
    if (!base) return NULL;
    char *marker = strstr(base, "_full.");
    if (marker) memmove(marker, marker + 5, strlen(marker + 5) + 1);

    char *variant = image_url_variant(base, "_micro");
    free(base);
    // _micro is always GIF (image-optimizer.sh), regardless of source format.
    if (variant) {
        char *dot = strrchr(variant, '.');
        if (dot) strcpy(dot, ".gif");
    }
    return variant;
}

static char *render_image_paragraph(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");
    char *tpl = load_template("elements/image-paragraph/image-paragraph_epoch%d.html", epoch);
    if (!tpl) return NULL;
    char align_attr[32] = "";
    const char *a = block->extra_data;
    if (a && (strcmp(a, "left") == 0 || strcmp(a, "right") == 0))
        snprintf(align_attr, sizeof(align_attr), "align=\"%s\"", a);
    char *src = image_paragraph_src(block->text, epoch);
    char *result = src ? render_template(tpl, src, align_attr) : NULL;
    free(src);
    free(tpl);
    return result ? result : strdup("");
}

// Epochs 0-1 cannot lay a real <table> out reliably, so a table block becomes
// a monospaced grid inside <pre> instead: box-drawn with '+'/'-'/'|', column
// widths measured from the cells themselves. WML (-1) already sidesteps the
// problem with a plain <p>/<br> layout and needs no such treatment.
// Counts display columns, not bytes: a UTF-8 continuation byte (10xxxxxx)
// extends the character before it rather than adding one of its own. Good
// enough for the accented Latin letters and symbols (×, degrees, etc.) this
// site's bilingual content actually uses - not a full East-Asian-width table,
// but byte length alone would misalign every column a Spanish "ó" touches.
static int utf8_width(const char *s) {
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) width++;
    return width;
}

static char *render_table_ascii(const CmsContentBlock *block, int epoch, int has_header) {
    char *tpl = load_template("elements/table/table_epoch%d.html", epoch);
    if (!tpl) return NULL;

    // Pass 1: split into rows/cells and remember every cell - the width of
    // column N cannot be known until every row has been read once.
    char ***rows = NULL;
    int *col_counts = NULL;
    int row_count = 0, row_cap = 0, max_cols = 0;

    char *copy = strdup(block->text);
    char *row_sp = NULL;
    for (char *row_tok = strtok_r(copy, "\n", &row_sp); row_tok;
         row_tok = strtok_r(NULL, "\n", &row_sp)) {
        while (*row_tok == '\r') row_tok++;
        char *end = row_tok + strlen(row_tok) - 1;
        while (end >= row_tok && (*end == '\r' || *end == '\n')) *end-- = '\0';
        if (!*row_tok) continue;

        if (row_count >= row_cap) {
            row_cap = row_cap ? row_cap * 2 : 8;
            rows       = realloc(rows, (size_t)row_cap * sizeof(char **));
            col_counts = realloc(col_counts, (size_t)row_cap * sizeof(int));
        }

        char **cells = NULL;
        int cell_cap = 0, cell_count = 0;
        char *row_copy = strdup(row_tok);
        char *cell_sp = NULL;
        for (char *ct = strtok_r(row_copy, "|", &cell_sp); ct; ct = strtok_r(NULL, "|", &cell_sp)) {
            if (cell_count >= cell_cap) {
                cell_cap = cell_cap ? cell_cap * 2 : 4;
                cells = realloc(cells, (size_t)cell_cap * sizeof(char *));
            }
            cells[cell_count++] = strdup(ct);
        }
        free(row_copy);

        rows[row_count] = cells;
        col_counts[row_count] = cell_count;
        if (cell_count > max_cols) max_cols = cell_count;
        row_count++;
    }
    free(copy);

    if (row_count == 0 || max_cols == 0) {
        for (int r = 0; r < row_count; r++) {
            for (int c = 0; c < col_counts[r]; c++) free(rows[r][c]);
            free(rows[r]);
        }
        free(rows); free(col_counts); free(tpl);
        return strdup("");
    }

    int *widths = calloc((size_t)max_cols, sizeof(int));
    for (int r = 0; r < row_count; r++)
        for (int c = 0; c < col_counts[r]; c++) {
            int len = utf8_width(rows[r][c]);
            if (len > widths[c]) widths[c] = len;
        }

    // "+----+----+" - reused as-is for the outer border and, when the block
    // has a header, as the rule under it.
    char *border = strdup("+");
    for (int c = 0; c < max_cols && border; c++) {
        int n = widths[c] + 2; // one space of padding on each side
        if (n > 120) n = 120;
        char dashes[122];
        memset(dashes, '-', (size_t)n);
        dashes[n] = '\0';
        char seg[128];
        snprintf(seg, sizeof(seg), "%s+", dashes);
        border = str_append(border, seg);
    }

    char *art = border ? str_append(strdup(border), "\n") : NULL;

    for (int r = 0; r < row_count && art; r++) {
        char *line = strdup("|");
        for (int c = 0; c < max_cols && line; c++) {
            const char *text = c < col_counts[r] ? rows[r][c] : "";
            // Manual padding: printf's `%-*s` counts bytes, and a multi-byte
            // UTF-8 character would then get fewer trailing spaces than its
            // neighbours in the same column need to line up.
            int pad = widths[c] - utf8_width(text);
            if (pad < 0) pad = 0;
            char cell[600];
            int n = snprintf(cell, sizeof(cell), " %s", text);
            if (n < 0) n = 0;
            if (n >= (int)sizeof(cell)) n = (int)sizeof(cell) - 1;
            for (int i = 0; i < pad && n < (int)sizeof(cell) - 2; i++) cell[n++] = ' ';
            cell[n++] = ' ';
            cell[n++] = '|';
            cell[n] = '\0';
            line = str_append(line, cell);
        }
        if (line) { art = str_append(art, line); art = str_append(art, "\n"); }
        free(line);

        if (has_header && r == 0 && art) {
            art = str_append(art, border);
            art = str_append(art, "\n");
        }
    }
    if (art) art = str_append(art, border);

    free(border);
    for (int r = 0; r < row_count; r++) {
        for (int c = 0; c < col_counts[r]; c++) free(rows[r][c]);
        free(rows[r]);
    }
    free(rows); free(col_counts); free(widths);

    char *result = art ? render_template(tpl, art) : NULL;
    free(art);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_table(const CmsContentBlock *block, int epoch) {
    if (!block->text || !block->text[0]) return strdup("");

    int has_header = (block->extra_data && strcmp(block->extra_data, "header") == 0);
    if (epoch == EPOCH_PRESTANDARD || epoch == EPOCH_EARLY)
        return render_table_ascii(block, epoch, has_header);

    // WML does have a real <table>, but it has no width or wrapping
    // properties whatsoever - on a narrow phone screen, a several-column
    // row renders as one very long line the reader has to scroll
    // horizontally to read at all (confirmed live in a WAP emulator), the
    // same "documented but not actually usable" problem already found with
    // Cello and images. Transposing each row into "label: value" lines
    // using the header row's own text as labels reads fine on any screen
    // width, since it never depends on one - a table with no header falls
    // back to one bare value per line, still unlabeled but still readable.
    if (epoch == EPOCH_WML) {
        char *lines = strdup(block->text);
        if (!lines) return NULL;

        char *rows[256];
        int row_count = 0;
        char *rsp = NULL;
        for (char *r = strtok_r(lines, "\n", &rsp); r && row_count < 256;
             r = strtok_r(NULL, "\n", &rsp)) {
            while (*r == '\r') r++;
            char *end = r + strlen(r) - 1;
            while (end >= r && *end == '\r') *end-- = '\0';
            if (*r) rows[row_count++] = r;
        }

        char *labels[64] = {0};
        int label_count = 0;
        int data_start = 0;
        if (has_header && row_count > 0) {
            char *hcopy = strdup(rows[0]);
            char *hsp = NULL;
            for (char *c = strtok_r(hcopy, "|", &hsp); c && label_count < 64;
                 c = strtok_r(NULL, "|", &hsp)) {
                labels[label_count++] = strdup(c);
            }
            free(hcopy);
            data_start = 1;
        }

        char *result = strdup("");
        for (int i = data_start; i < row_count && result; i++) {
            char *record = strdup("");
            char *rcopy = strdup(rows[i]);
            char *csp = NULL;
            int col = 0;
            for (char *c = strtok_r(rcopy, "|", &csp); c && record;
                 c = strtok_r(NULL, "|", &csp), col++) {
                char *line = (col < label_count)
                    ? render_template("%s: %s<br/>", labels[col], c)
                    : render_template("%s<br/>", c);
                record = line ? str_append(record, line) : NULL;
                free(line);
            }
            free(rcopy);

            char *wrapped = record ? render_template("<p>%s</p>", record) : NULL;
            free(record);
            result = wrapped ? str_append(result, wrapped) : NULL;
            free(wrapped);
            if (result) result = str_append(result, "<p><br/></p>");
        }

        for (int i = 0; i < label_count; i++) free(labels[i]);
        free(lines);
        return result ? result : strdup("");
    }

    char *table_tpl  = load_template("elements/table/table_epoch%d.html",             epoch);
    char *row_tpl    = load_template("elements/table/table-row_epoch%d.html",          epoch);
    char *cell_tpl   = load_template("elements/table/table-cell_epoch%d.html",         epoch);
    char *header_tpl = load_template("elements/table/table-header-cell_epoch%d.html",  epoch);
    if (!table_tpl || !row_tpl || !cell_tpl || !header_tpl) {
        free(table_tpl); free(row_tpl); free(cell_tpl); free(header_tpl);
        return strdup("");
    }

    char *rows_html = strdup("");
    char *copy = strdup(block->text);
    char *row_sp = NULL;
    int row_idx = 0;

    for (char *row_tok = strtok_r(copy, "\n", &row_sp);
         row_tok && rows_html;
         row_tok = strtok_r(NULL, "\n", &row_sp), row_idx++) {
        while (*row_tok == '\r') row_tok++;
        char *end = row_tok + strlen(row_tok) - 1;
        while (end >= row_tok && (*end == '\r' || *end == '\n')) *end-- = '\0';
        if (!*row_tok) continue;

        char *cell_tpl_cur = (has_header && row_idx == 0) ? header_tpl : cell_tpl;
        char *cells = strdup("");
        char *row_copy = strdup(row_tok);
        char *cell_sp  = NULL;
        for (char *ct = strtok_r(row_copy, "|", &cell_sp); ct && cells; ct = strtok_r(NULL, "|", &cell_sp)) {
            char *cell = render_template(cell_tpl_cur, ct);
            cells = cell ? str_append(cells, cell) : NULL;
            free(cell);
        }
        free(row_copy);
        char *row = cells ? render_template(row_tpl, cells) : NULL;
        rows_html = row ? str_append(rows_html, row) : NULL;
        free(cells); free(row);
    }
    free(copy);

    char *result = rows_html ? render_template(table_tpl, rows_html) : NULL;
    free(rows_html);
    free(table_tpl); free(row_tpl); free(cell_tpl); free(header_tpl);
    return result ? result : strdup("");
}

static char *render_social_networks(const CmsContentBlock *block, int epoch) {
    if (!block->extra_data || !block->extra_data[0]) return strdup("");
    char *pipe = strchr(block->extra_data, '|');
    if (!pipe) return strdup("");

    char *tpl = load_template("elements/social-networks/social-networks_epoch%d.html", epoch);
    if (!tpl) return NULL;

    char icon[64] = {0};
    size_t icon_len = (size_t)(pipe - block->extra_data);
    if (icon_len >= sizeof(icon)) icon_len = sizeof(icon) - 1;
    strncpy(icon, block->extra_data, icon_len);
    const char *url = pipe + 1;

    // Icon format follows the epoch: SVG only for modern browsers, GIF for the
    // ones that predate it (a small variant for epoch 1), and no icon at all
    // for text-only/WML clients, whose templates keep it in a comment anyway.
    char icon_path[256] = "";
    if (epoch >= EPOCH_MODERN)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s.svg", icon);
    else if (epoch == EPOCH_MIDDLE)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s.gif", icon);
    else if (epoch == EPOCH_EARLY)
        snprintf(icon_path, sizeof(icon_path),
                 "/themes/dark/assets/social-networks/%s-s.gif", icon);

    const char *name = (block->text && block->text[0]) ? block->text : icon;
    char *result = render_template(tpl, url, icon_path, name, name);
    free(tpl);
    return result ? result : strdup("");
}

static char *render_block(const CmsContentBlock *block, int epoch) {
    if (strcmp(block->type, "title") == 0)          return render_title(block, epoch);
    if (strcmp(block->type, "paragraph") == 0)       return render_paragraph(block, epoch);
    if (strcmp(block->type, "byline") == 0)          return render_byline(block, epoch);
    if (strcmp(block->type, "image") == 0)           return render_image(block, epoch);
    if (strcmp(block->type, "gallery") == 0)         return render_gallery(block, epoch);
    if (strcmp(block->type, "separator") == 0)       return render_separator(block, epoch);
    if (strcmp(block->type, "link") == 0)            return render_link(block, epoch);
    if (strcmp(block->type, "list") == 0)            return render_list(block, epoch);
    if (strcmp(block->type, "youtube-embed") == 0)   return render_youtube_embed(block, epoch);
    if (strcmp(block->type, "code-text") == 0)       return render_code_text(block, epoch);
    if (strcmp(block->type, "generic") == 0)         return render_generic(block, epoch);
    if (strcmp(block->type, "image-paragraph") == 0) return render_image_paragraph(block, epoch);
    if (strcmp(block->type, "table") == 0)           return render_table(block, epoch);
    if (strcmp(block->type, "social-networks") == 0) return render_social_networks(block, epoch);
    return strdup("");
}

// Renders every block individually rather than concatenating as it goes, so
// WML pagination (see entry_page()) can group whole blocks onto a page
// without ever cutting one in half. *count_out is only meaningful when this
// returns non-NULL. Frees anything already rendered and returns NULL on the
// first failure, matching entry_page_render_content()'s existing contract.
static char **render_blocks(const CmsEntry *entry, int epoch, size_t *count_out) {
    char **blocks = calloc(entry->content_count ? entry->content_count : 1, sizeof(char *));
    if (!blocks) return NULL;

    for (size_t i = 0; i < entry->content_count; i++) {
        blocks[i] = render_block(&entry->content[i], epoch);
        if (!blocks[i]) {
            for (size_t j = 0; j < i; j++) free(blocks[j]);
            free(blocks);
            return NULL;
        }
    }

    *count_out = entry->content_count;
    return blocks;
}

static void free_blocks(char **blocks, size_t count) {
    for (size_t i = 0; i < count; i++) free(blocks[i]);
    free(blocks);
}

char *entry_page_render_content(const CmsEntry *entry, int epoch) {
    size_t count = 0;
    char **blocks = render_blocks(entry, epoch, &count);
    if (!blocks) return NULL;

    char *result = strdup("");
    for (size_t i = 0; i < count && result; i++) {
        result = str_append(result, blocks[i]);
    }
    free_blocks(blocks, count);
    return result;
}

// A real WAP 1.x deck has to fit in a few KB of device memory (a Nokia 7110
// topped out around 1400 *compiled* bytes), and compilation only shrinks
// raw markup so much - 2000 raw bytes/page is a rough but reasonable target,
// live-tested against a real emulator's own ~16KB ceiling with plenty of
// margin to spare rather than tuned to just barely fit it.
#define WML_PAGE_BUDGET 2000

char *entry_page(const CmsEntry *entry, int epoch, int page, int *total_pages_out) {
    char *categories_html = render_categories(entry, epoch);
    if (!categories_html) return NULL;

    char *meta_html;
    const char *author = entry->header_author ? entry->header_author : "";
    // With hide_author set (or no author at all) the byline is dropped entirely
    // rather than emitted empty, so the entry starts at its content. Every
    // epoch has an entry-meta template; the older ones just say it plainly.
    if (!entry->header_hide_author && author[0]) {
        char *meta_tpl = load_template("entry/entry-meta_epoch%d.html", epoch);
        meta_html = meta_tpl ? render_template(meta_tpl, author, categories_html) : categories_html;
        free(meta_tpl);
        if (meta_tpl) free(categories_html);
    } else {
        meta_html = categories_html;
    }

    if (!meta_html) return NULL;

    // WML has no block-level separation of its own between two chunks of
    // markup concatenated back to back (no div/margins - CSS concepts it
    // predates), so without a blank-line spacer the byline/category line
    // ran straight into the title with no visible gap in a real WAP
    // emulator.
    if (epoch == EPOCH_WML) {
        char *spaced = str_append(meta_html, "<p><br/></p>");
        if (spaced) meta_html = spaced;
    }

    if (epoch != EPOCH_WML) {
        if (total_pages_out) *total_pages_out = 1;
        char *content_html = entry_page_render_content(entry, epoch);
        if (!content_html) {
            free(meta_html);
            return NULL;
        }
        char *result = str_append(meta_html, content_html);
        free(content_html);
        return result;
    }

    // --- WML: group whole blocks into decks under WML_PAGE_BUDGET ---
    size_t count = 0;
    char **blocks = render_blocks(entry, epoch, &count);
    if (!blocks) {
        free(meta_html);
        return NULL;
    }

    // page_starts[k] = index into blocks[] where page k+1 begins. Page 1
    // always exists even if content_count == 0 (just the meta/title).
    size_t *page_starts = malloc((count + 1) * sizeof(size_t));
    int total_pages = 0;
    if (!page_starts) {
        free_blocks(blocks, count);
        free(meta_html);
        return NULL;
    }

    page_starts[total_pages++] = 0;
    size_t running = strlen(meta_html);
    for (size_t i = 0; i < count; i++) {
        int at_page_start = (i == page_starts[total_pages - 1]);
        if (!at_page_start && running + strlen(blocks[i]) > WML_PAGE_BUDGET) {
            page_starts[total_pages++] = i;
            running = 0;
        }
        running += strlen(blocks[i]);
    }

    if (total_pages_out) *total_pages_out = total_pages;
    int cur = page < 1 ? 1 : (page > total_pages ? total_pages : page);

    size_t start = page_starts[cur - 1];
    size_t end   = (cur < total_pages) ? page_starts[cur] : count;

    char *result = strdup(cur == 1 ? meta_html : "");
    for (size_t i = start; i < end && result; i++) {
        result = str_append(result, blocks[i]);
    }

    // [Prev]/[Next], entry->type/link building the same "/blog/<link>" or
    // "/page/<link>" URL the route itself answers to.
    if (result && total_pages > 1) {
        char nav[600] = "<p>";
        char piece[300];
        if (cur > 1) {
            snprintf(piece, sizeof(piece), "[<a href=\"/%s/%s?page=%d\">Prev</a>] ",
                      entry->type, entry->link, cur - 1);
            strncat(nav, piece, sizeof(nav) - strlen(nav) - 1);
        }
        if (cur < total_pages) {
            snprintf(piece, sizeof(piece), "[<a href=\"/%s/%s?page=%d\">Next</a>] ",
                      entry->type, entry->link, cur + 1);
            strncat(nav, piece, sizeof(nav) - strlen(nav) - 1);
        }
        snprintf(piece, sizeof(piece), "(%d/%d)</p>", cur, total_pages);
        strncat(nav, piece, sizeof(nav) - strlen(nav) - 1);
        result = str_append(result, nav);
    }

    free(page_starts);
    free_blocks(blocks, count);
    free(meta_html);
    return result;
}
