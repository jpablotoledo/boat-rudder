# Theme System: `templates/` + `themes/` Split, DB-Backed Colors - Implementation Plan

> **Status**: implemented as described in §2-§6, migration step §7 items 1-4 and 6 done (`page-home`
> stayed theme-owned per §8.1's lean, `assets/social-networks`/`assets/blog-list` not moved per
> §8.2, `dashboard/` stayed inside `templates/` per §8.3). Verified end-to-end: every route
> (`/`, `/blog`, `/blog/category/<slug>`, `/blog/<link>`, `/page/<link>`, `/gallery/<id>`,
> `/login`, `/dashboard` and its sub-pages, a 404) across all five epochs, plus
> `/dashboard/settings/themes`'s activate/colors round-trip, live CSS-variable propagation, and
> "clear it, get the hardcoded default back" for a fresh `themes` document. §8.4/§8.5 (the
> preview tool's theme selector, the `access()` cost) remain open, non-blocking follow-ups.
>
> **Epoch 1/2 colors, then unified with epoch 3's** (two follow-up requests, not originally
> scoped here): first added as a *separate* palette (`CmsRetroColors`/`retro_colors`, its own
> form) for epochs with no CSS custom properties - verified against the actual templates rather
> than assumed: epoch 1 and 2 already shared identical hardcoded hex for
> background/text/accent/author/date/category; `border` was epoch-2-only (the home-blog item's
> table `bordercolor` - epoch 1's list has no box, just an `<hr>`). A second request then asked
> for *one* set of controls affecting every epoch instead of a settings page split by epoch, so
> `CmsRetroColors` was folded into `CmsThemeColors` (now 7 fields: background, text, accent,
> author, date, category, border) and `cms_get_retro_colors()`/`cms_update_retro_colors()`
> removed - epoch 3 reads it as CSS variables, epoch 1/2 (no CSS custom properties available)
> get the same values substituted straight into `bgcolor`/`text`/`link`/`vlink`/
> `<font color>`/`style="color:..."`/`bordercolor` attributes. One consequence, disclosed and
> accepted: epoch 1/2's fresh-install colors changed from their own prior muted palette
> (teal/pink/grey/purple) to epoch 3's neon one (cyan/yellow/orange/green), since one shared
> default set can no longer reproduce two different epochs' historical hex simultaneously.
> `/dashboard/settings/themes` now has a single 7-field color form per theme, not two.
>
> Written after
> [site-personalization-plan.md](site-personalization-plan.md) shipped (site name, raw-HTML
> banner/footer per epoch, theme-asset uploads, `/dashboard/settings/preview`, the navbar's
> signed-in-user link) - that work is unaffected by this plan and is described in its own
> document. This plan **supersedes the "Colors" section** of
> [site-settings-plan.md](site-settings-plan.md) (§3, §7): a color palette is a property of a
> *theme*, not of the site's identity, so it moves from `site_settings.colors` to the new
> `themes` collection this document defines.
>
> **§9: a second theme (`light`), a per-visitor theme switcher, and the fixes that took to get
> there** - see §9 below for the full analysis. Short version: getting a real second theme
> working surfaced a real bug (`cms_get_theme_colors()`'s fallback was a single hardcoded
> palette shared by every theme key, so a theme with no saved document inherited `dark`'s neon
> colors instead of its own), now fixed with a per-theme default table. The navbar gained a
> theme switcher mirroring the language one exactly, including per-visitor choice via a `theme`
> cookie (`request_theme()`'s precedence is now query > cookie > `site_settings.active_theme` >
> config, identical in shape to `request_lang()`). `light` itself is a **mechanical** conversion,
> not a bespoke redesign - see §9.3 for exactly what that means and what's still rough.
>
> **Follow-up**: `light` existing also exposed that the home banner and footer
> (site-personalization-plan.md) were still site-wide, not per-theme like colors - see
> [theme-scoped-personalization-plan.md](theme-scoped-personalization-plan.md), which moves them
> into `themes.<key>` and reshapes `/dashboard/settings` so a theme's colors, banner and footer
> are edited together.

## 1. Goal

Today `html/themes/dark/` is one directory holding ~150 template files, and it mixes two things
that have nothing to do with each other:

1. **The retro-compatibility engine's own rendering rules** - how a `paragraph`/`gallery`/`table`
   content block looks in epoch 1 vs epoch 3, how the login form or an error page is laid out,
   the entire `/dashboard` admin tool. This is Boat Rudder's product logic. It should be
   identical for every site running Boat Rudder, and a site owner should never need to touch it
   to get a working install.
2. **This particular site's visual identity** - the nav bar, the home banner, the page chrome
   (`layout/`), the color palette. This is what "dark" *is*: a specific, opinionated retro-neon
   skin. A different site (or the same site redesigned) would want a different one, without
   reimplementing how a gallery block degrades on Netscape 4.

Because everything lives under one `html/themes/<theme>/` tree today, "add a theme" means
copying and maintaining a parallel ~150-file tree, 95% of which (every content block, the entire
admin UI) would be byte-for-byte identical to `dark`'s. That's the problem this plan fixes:
split the tree into a shared, theme-agnostic **`html/templates/`** and a slim, brand-only
**`html/themes/<theme>/`**, and make each theme's design also carry a handful of DB-backed color
tokens editable from a new **`/dashboard/settings/themes`** section - the "hybrid" solution
requested: a theme is *both* an on-disk override tree *and* a database record.

## 2. Current state: classifying every directory

`find html/themes/dark -type f` lists 29 top-level component directories plus `assets/` and
`styles_epoch3.css`. None of this is arbitrary - every file already has a clear home once sorted
by "does swapping the theme plausibly change this":

| Directory | Files (~) | Classification | Why |
|---|---|---|---|
| `menu/` | 25 | **theme** | Nav bar structure, logo, brand link - explicitly named in the request |
| `mainbanner/` | 5 | **theme** | The home banner (`mainbanner`) - explicitly named |
| `layout/` | 13 | **theme** | Whole-document chrome: doctype, `<head>`, `styles_epoch3.css` link, footer/lightbox/home-modal splice points, body background classes - explicitly named |
| `category-menu/` | 20 | **theme** | A second nav bar (category filter) - same reasoning as `menu/` |
| `home-content/` | 10 | **theme** | The homepage's hand-written "Welcome" section - home page composition, not generic content rendering |
| `home-blog/` | 10 | **theme** | The homepage's "Latest Blog Posts" widget layout/heading - same reasoning |
| `page/page-home_epoch*.html` | 5 | **theme** | Composes menu+mainbanner+home-content+home-blog into the home page's shape - arguably borderline (see §8.1) |
| `styles_epoch3.css` | 1 | **theme** | The visual design itself - ~76 hardcoded hex colors, exactly what §5 below layers DB tokens onto |
| `assets/mainbanner/`, `assets/footer/`, `assets/menu/`, `assets/home-content/` | ~20 | **theme** | Brand imagery: the actual banner photo, footer graphic, nav logo - not swappable without also changing the design |
| `page/page_epoch*.html`, `page-entry_epoch*.html`, `page-blog_epoch*.html` | 9 | **template** | Content-area shells (nav + content, or + category bar) - two `%s` slots, no brand-specific markup |
| `elements/<14 block types>/` | ~65 | **template** | How a `title`/`paragraph`/`image`/`gallery`/`table`/... content block renders per epoch - the CMS's own rendering rules, reused by every entry regardless of theme |
| `entry/` | 10 | **template** | Entry metadata (categories, author/date byline) rendering |
| `error/` | 5 | **template** | `error_epoch<N>.html` for every non-2xx/3xx status |
| `login/` | 6 | **template** | The login form and its epoch-gated "not available" message |
| `language/` | 10 | **template** | The `/language` fallback page (epoch 0/1 language switch) |
| `redirect/` | 5 | **template** | The generic redirect page |
| `dashboard/` | ~55 | **template** | The entire admin tool (entries, categories, languages, menu, users, media, settings) - Boat Rudder's own UI, epoch-3-only, never reskinned per site |
| `assets/social-networks/` | 24 | **template** (candidate) | Generic platform icons (Facebook/GitHub/...) - not brand-specific; see §8.2 |
| `assets/blog-list/` | 2 | **template** (candidate) | Tiny epoch-1/-1 list-separator pixels used by generic list rendering; see §8.2 |

Net effect: **`html/templates/` absorbs roughly two-thirds of the current file count** (every
content-block renderer plus the entire admin tool), and `html/themes/dark/` shrinks to the
~75 files that are actually "dark"'s design - menu, banner, chrome, category bar, home
composition, CSS, and brand imagery.

## 3. Resolution: hybrid lookup, theme overrides templates

A theme is not required to carry every file a full skin might - most themes should only need to
override `menu/`, `mainbanner/`, `layout/`, `category-menu/`, `home-content/`, `home-blog/`, and
`styles_epoch3.css`. But nothing stops a theme from *also* overriding something that normally
lives in `templates/` (a theme with a wildly different admin skin, say). Hence "hybrid": every
template lookup checks the active theme first, and falls back to the shared tree only if the
theme doesn't have its own copy.

`src/utils/generate_url_theme.c` is the single choke point every module already goes through
(`generate_url_theme("menu/menu_epoch%d.html", epoch)` -> a path string), so this is a
**one-function change**, invisible to every one of its ~25 call sites:

```c
// Before: always "./html/themes/<theme>/<subpath>".
// After: "./html/themes/<theme>/<subpath>" if that file exists, otherwise
// "./html/templates/<subpath>" - callers don't know or care which one they
// got back.
char *generate_url_theme(const char *subpath_fmt, int epoch) {
    char subpath[256];
    /* ...unchanged: build subpath from subpath_fmt + epoch... */

    char *theme_path = /* "./html/themes/<theme>/" + subpath */;
    if (access(theme_path, F_OK) == 0) return theme_path;
    free(theme_path);

    return /* "./html/templates/" + subpath */;
}
```

This costs one extra `access()` syscall per template load in the common case (a file the theme
does *not* override) - consistent with the project's existing no-caching, read-per-request
convention (menu's DB read, `cms_get_site_settings()`, etc. all already accept this trade-off;
see §8.3 for when that stops being fine).

Static assets (`assets/...`) go through the **same** two-directory hybrid, but at the HTTP layer
instead of `generate_url_theme()`: `serve_static_file()` already resolves `/themes/<theme>/...`
requests against `html/themes/<theme>/`; it gains one fallback check against
`html/templates/assets/...` before returning 404. A theme-owned image
(`/themes/<theme>/assets/mainbanner/epoch3/main.jpg`) and a hypothetical shared one
(`/themes/<theme>/assets/social-networks/facebook.svg`, physically living in
`html/templates/assets/social-networks/`) both resolve through the same route, so entry content
and templates keep referencing image paths exactly as they do today - no URL scheme change, only
where the file physically lives.

## 4. Which theme is "active": request-scoped, DB-backed, config-fallback

`configs/settings.conf`'s `theme=dark` key today is a static, process-lifetime value
(`extern char theme[64]`) read directly by `generate_url_theme()`. This plan keeps that key as
the **fallback default** - same role `lang` already plays for content language - and adds a
DB-backed override: `site_settings.active_theme`, editable from `/dashboard/settings/themes`.
"Active theme" is site *identity* (which skin this site is currently wearing), so it belongs
next to `site_name` in the existing `site_settings` singleton, not in the new `themes` catalog
(§5) - the same distinction [site-personalization-plan.md](site-personalization-plan.md) draws
between site content and design.

```c
// src/db/cms_site_settings.h - one new field, one new getter/setter
typedef struct {
    char site_name[128];
    char active_theme[64];   // "" = use configs/settings.conf's `theme`
    char *banner_html[SITE_SETTINGS_EPOCH_COUNT];
    char *footer_html[SITE_SETTINGS_EPOCH_COUNT];
} CmsSiteSettings;

// Returns a malloc'd theme key: site_settings.active_theme if set and its
// directory exists under html/themes/, otherwise configs/settings.conf's
// `theme`. Never NULL, never a key with no matching directory.
char *cms_get_active_theme_key(void);

int cms_set_active_theme(const char *key); // validates the directory exists
```

`generate_url_theme()` needs this value on **every** call (several layers deep in `menu()`,
`mainbanner()`, `entry_page.c`, ...), so - mirroring `request_lang.h`/`request_user.h`'s established
"cross-cutting per-request value, resolved once, stored per-thread" pattern - a new
`src/utils/request_theme.h/.c` resolves it once per request:

```c
// Resolves the active theme (DB, falling back to configs/settings.conf) and
// stores it for request_theme(). Call once per request, alongside
// request_lang_set()/request_user_set() in http_router.c.
void request_theme_set(void);

// The theme key resolved by the last request_theme_set() on this thread.
// Never NULL/empty. generate_url_theme() calls this instead of reading the
// `theme` global directly.
const char *request_theme(void);
```

This is one more per-request DB read alongside the language/session ones already there - same
accepted cost, same reasoning (§8.3 covers when to revisit).

## 5. Proposed schema: `themes` collection (one document per customized theme)

Unlike `site_settings` (a true singleton), `themes` has **one sparse document per theme key that
an admin has actually customized** - a theme with no document just uses `styles_epoch3.css`'s
hardcoded colors, exactly like today. There is no "catalog"/registration step: `GET
/dashboard/settings/themes` discovers available themes by `readdir()`-ing `html/themes/` (same
technique already used for the theme-asset browser in `site-personalization-plan.md` §6), and
looks up each one's color document by key.

```jsonc
// db.themes - one document per customized theme, key is unique
{
  "_id": ObjectId,
  "key": "dark",              // matches the html/themes/<key>/ directory name
  "colors": {
    "accent": "#7b0a74",
    "background": "#000000",
    "text": "#ffffff"
  }
}
```

Started at the 3-color scope [site-settings-plan.md §3](site-settings-plan.md) already proposed
(accent/background/text), then grew to 6 once a real customization pass over the home blog
listing asked for its own tokens: `border` (the listing item's border, reused for
`.boat-rudder__entry-category`'s badge background - both were already the same hardcoded hex),
`author` and `date` (the byline). That plan's open question #1 ("confirm 3 colors is right") is
answered: not a fixed number - each token exists because a specific, already-hardcoded, reused
hex value asked for one. The set grows the same way going forward, not by pre-declaring a
larger fixed palette up front.

### DB layer: `src/db/cms_themes.h/.c`

```c
typedef struct {
    char accent[8];      // "#rrggbb"
    char background[8];
    char text[8];
} CmsThemeColors;

// db.themes.findOne({key}). Fills `out` with the stored colors, or with
// today's hardcoded styles_epoch3.css values if no document exists for
// `key` or mongodb is unreachable - a theme with no saved colors must
// render exactly as it does today, same fallback discipline as everything
// else in this codebase. Always returns 1.
int cms_get_theme_colors(const char *key, CmsThemeColors *out);

// db.themes.updateOne({key}, {$set: {colors}}, {upsert: true}).
// Returns 0 on success, -1 on a DB error or if mongodb is not ready.
int cms_update_theme_colors(const char *key, const CmsThemeColors *colors);
```

### Rendering: the same CSS-custom-property mechanism already designed, re-sourced

[site-settings-plan.md §7](site-settings-plan.md) already worked out the mechanism - it just
sourced it from the wrong collection. Unchanged: `layout_epoch3.html` (or wherever the `<head>`
is assembled) gains a small inline fragment,

```html
<style>:root{ --br-color-accent: %s; --br-color-background: %s; --br-color-text: %s; }</style>
```

filled from `cms_get_theme_colors(request_theme(), &colors)`, and `styles_epoch3.css` rules opt
in incrementally via `var(--br-color-accent, <current-hardcoded-value>)` - the fallback value
keeps every untouched rule working before (and after) any given rule is migrated to the
variable. Epoch -1/0/1/2 are out of scope for the same reasons already given in
site-settings-plan.md §3 (no color model, or a separate inline-attribute mechanism not addressed
here).

## 6. Proposed admin UI: `/dashboard/settings/themes`

Follows the same shape as the banner/footer editors in
[site-personalization-plan.md §7](site-personalization-plan.md): one panel per discovered theme,
admin-only (`role == "admin"`).

- New module function `site_settings_themes_page(epoch, const char *active_key, const ThemeEntry *themes, size_t count)` in the existing `site_settings_admin` module (folding in here rather than a new module, same reasoning as that plan's own §9.1 - one "Site settings" nav entry, shared guard).
- Each panel: theme key/name, a "Set active" button (disabled/marked if already active), and a small form with one `<input type="color">` per token (accent/background/text/border/author/date) + "Save colors" - mirrors site-settings-plan.md §6's original color-picker UX proposal, unchanged.
- Routes (admin-gated, form POST + redirect - same convention as every other settings sub-page):

  | Route | Method | Behavior |
  |---|---|---|
  | `GET /dashboard/settings/themes` | GET | `readdir("./html/themes/")` for available keys, `cms_get_theme_colors(key, &colors)` per key, `site_settings.active_theme` (or config fallback) for which is active. |
  | `POST /dashboard/settings/themes/<key>/activate` | POST | Validates `html/themes/<key>/` exists, `cms_set_active_theme(key)`, redirect back. |
  | `POST /dashboard/settings/themes/<key>/colors` | POST | Parses the 3 color fields, `cms_update_theme_colors(key, &colors)`, redirect back. |

- `/dashboard/settings` gains a "Themes" link alongside Banner/Footer/Preview.
- `/dashboard/settings/preview` (already shipped) is unaffected in behavior, but becomes strictly
  more useful here: previewing an epoch already re-renders through the same
  `generate_url_theme()`/`request_theme()` path, so once a second theme exists, the preview tool
  is the natural place to compare themes too - worth a follow-up "theme" selector alongside the
  existing epoch/screen-size ones, noted as an open question (§8.4) rather than in this plan's
  concrete scope.

## 7. Migration: moving today's files, not rewriting them

This is a `git mv` exercise, not a rewrite - every file's *content* is untouched, only its path
changes, per the classification in §2:

1. Create `html/templates/`.
2. `git mv` each **template**-classified directory (§2) from `html/themes/dark/` to
   `html/templates/`: `page/page_epoch*`, `page/page-entry_epoch*`, `page/page-blog_epoch*`
   (not `page-home_epoch*` - stays), `elements/`, `entry/`, `error/`, `login/`, `language/`,
   `redirect/`, `dashboard/`.
3. Leave every **theme**-classified directory in place under `html/themes/dark/`: `menu/`,
   `mainbanner/`, `layout/`, `category-menu/`, `home-content/`, `home-blog/`,
   `page/page-home_epoch*`, `styles_epoch3.css`, and the brand-imagery subset of `assets/`.
4. `generate_url_theme()` ships with its fallback (§3) *before* step 2 runs, so the migration
   can happen file-group by file-group with the site fully working the whole time (a template
   not yet moved still resolves from `html/themes/dark/` exactly as today; one already moved
   resolves from `html/templates/` - no window where anything 404s).
5. `serve_static_file()`'s fallback (§3) ships the same way for `assets/social-networks/` and
   `assets/blog-list/`, if §8.2 is resolved in favor of moving them.
6. Update every reference doc that documents the current single-tree layout -
   [architecture.md](../reference/architecture.md), [rendering.md](../reference/rendering.md),
   and [boat-rudder.md §2.3](../boat-rudder.md#23-templates-one-file-per-component-per-epoch) -
   to describe the two-tree layout.
7. A second theme is **not** required to prove this out - `dark` alone, split across two
   directories with `generate_url_theme()`'s fallback verified request-by-request (force each
   epoch via `force_epoch`, confirm identical output before/after each moved group), is
   sufficient acceptance. A second theme is a natural follow-up demo, not a blocker.

## 8. Open questions

1. **`page-home_epoch*.html`'s classification (§2)**: kept as theme-owned here because it
   composes the banner/menu/home-content/home-blog regions - the home page's *shape* feels like
   a design decision. But the file itself is nearly content-free (four `%s` slots, minimal
   markup around them), which argues for template. Worth a second look before step 2 of the
   migration touches it - low cost to move later either way, since it's a single 5-file group.
2. **`assets/social-networks/` and `assets/blog-list/` (§2)**: proposed as template-owned
   (generic/shared) but not migrated in this plan's first pass, to keep the initial move
   mechanical and low-risk. A follow-up once the main split is stable and a second theme exists
   to actually exercise "does every theme need its own Facebook icon" in practice.
3. **Should `dashboard/` eventually leave both trees**, living at a third top-level
   `html/dashboard/` instead of inside `templates/`? It is arguably not a "template" in the same
   sense as an `elements/paragraph` renderer - it's Boat Rudder's own tool, never themed, never
   reused as content. Classifying it under `templates/` (§2) is the smaller, less disruptive
   move for this plan; a later split into its own tree is possible without touching anything
   else once `generate_url_theme()`'s fallback chain exists.
4. **`/dashboard/settings/preview`'s theme selector**: once a second theme exists, comparing
   themes side-by-side (not just epochs/screen sizes) is an obvious extension of the tool
   shipped in site-personalization-plan.md - noted in §6, not scoped here since it depends on a
   second theme actually existing to be testable.
5. **The extra `access()` per template load (§3)**: accepted as consistent with the project's
   existing no-caching convention. If this becomes measurable (it wasn't a concern for the
   per-request DB reads this codebase already does, so unlikely to be one for a local stat()
   call), the fix is an isolated optimization - cache each theme's file listing at startup or on
   first miss - that doesn't change this plan's schema, resolution order, or admin UI.

## 9. A second theme (`light`) and a navbar theme switcher

Requested together: "analyse what a second theme actually needs" plus "a theme picker in the
navbar, like the language one." Building a real second theme is what surfaced the analysis - the
first attempt (just dropping a `light/` directory in) immediately showed a design gap this
document hadn't accounted for.

### 9.1 The bug: one hardcoded color fallback, not one per theme

`cms_get_theme_colors()` (§5) fell back to a single compile-time `DEFAULT_COLORS` struct - dark's
own hex values - whenever a theme had no saved `themes.<key>.colors` document. That's fine for
`dark` (its fallback *is* its own palette), but wrong for any other theme: a fresh `light` theme
with no customization yet would have inherited dark's neon cyan/yellow/orange/green instead of
anything resembling "light." The single-struct design implicitly assumed there would only ever
be one theme, which defeated the point of adding a second one.

**Fix**: `DEFAULT_COLORS` became `THEME_DEFAULTS[]`, a small array of `{key, CmsThemeColors}`
pairs (`src/db/cms_themes.c`) - `dark` and `light` each get their own entry; a theme with no
entry falls back to `dark`'s (the same "never break, never look broken" guarantee as everywhere
else in this codebase, just now keyed instead of singular). Epoch 3 doesn't strictly need this
table at all - its own `styles_epoch3.css` can carry `var(--x, <its-own-fallback>)` independent
of any C constant - but epoch 1/2 substitute plain hex directly into HTML attributes with no
`var()`-style local fallback mechanism available, so *something* server-side has to supply a
default per theme. This table is that something.

### 9.2 The switcher: a per-visitor cookie, mirroring language exactly

"Similar a la lista de idiomas" was taken literally: `request_theme()`'s precedence is now
`?theme=` query > `theme` cookie > `site_settings.active_theme` > `configs/settings.conf`,
structurally identical to `request_lang()`'s `?lang=` > `lang` cookie > `cms_resolve_default_lang()`.
This is a deliberate reframing from §4's original design, where "active theme" was purely a
site-wide admin setting - it still is *the default*, but a visitor can now override it for
themselves, the same way they already can with language. `/theme/set?key=<key>&return=<path>`
mirrors `/language/set` down to the plain-GET-so-it-works-with-no-JS rationale and reuses
`language_page.c`'s `language_sanitize_return()` (an open-redirect guard with nothing
language-specific in it despite the name).

The navbar control itself (`menu.c`'s `theme_selector()`, `menu-theme_epoch3.html`/
`menu-theme-item_epoch3.html`) is a line-for-line mirror of `language_selector()`: a drop-down,
hidden when fewer than two themes exist, listing whatever `list_theme_keys()` finds under
`html/themes/` (extracted from `http_router.c`'s admin-page code into
`src/utils/theme_catalog.c`, now shared by both).

**Update (follow-up request): epoch 1/2 got the same fallback language already has.** Originally
scoped to epoch 3 only - "a second full page for a control that mostly changes epoch 3's
typography/imagery wasn't judged worth it yet." A direct follow-up asked for it anyway, so
`menu-theme_epoch{1,2}.html` (a plain link, same shape as `menu-lang_epoch{1,2}.html`) now points
to a new `/theme` page (`src/modules/theme_page/theme_page.c`, `html/templates/theme/`) - a
line-for-line mirror of `/language` (`language_page.c`), including its epoch split: epoch 1 links
directly to `<return>?theme=xx` (no redirect - `request_theme_set()`'s query-string precedence,
already top of the chain per §9.2 above, picks it up with no extra plumbing needed), epoch 2
links to `/theme/set` (cookie-setting redirect). Epoch -1/0 remain out of scope - not requested,
and `/theme` itself gracefully 500s there (no template) rather than crashing, same as any other
missing-template path in this codebase; nothing links to it from those epochs so the gap is
inert.

`theme_key_is_valid()` (new, `theme_catalog.c`) gates every theme key from here on - the cookie,
the query string, and `cms_set_active_theme()`'s admin form - restricting it to
`[a-zA-Z0-9_-]` before ever calling `stat()`. Worth calling out: this closes a real (if narrow)
path-traversal gap that predates this section - `key=../content` would previously have resolved
to a real, unrelated directory under `html/` and passed the existence check, and unlike the
admin-only activation endpoint, `/theme/set` is public.

### 9.3 `light`: a mechanical conversion, not a redesign

`html/themes/light/` exists and works, built by copying `dark/` wholesale and then:

- **Every `/themes/dark/` reference rewritten to `/themes/light/`** (asset paths, the
  stylesheet `<link>`) across all copied files, so the theme is self-contained - it does not
  reach into `dark/`'s files or assets at runtime.
- **`styles_epoch3.css` had its dominant structural colors inverted** by a scripted find/replace
  over a handful of values that appear dozens of times each (`#000000` and its translucent
  `#0000006b`-style variants -> white; `#ffffff`/`#fff` -> dark text; the near-black card/panel
  greys `#1a1a1a`/`#1e1e1e`/`#0d0d0d`/`#111`/`#2a2a2a`/`#3a3a3a` -> light equivalents). Smaller,
  rarer accent colors (the neon cyan/purple/blue/pink/orange scattered through specific
  components) were **not** touched - some of them read fine as accents on a light background as
  a happy accident, some (light text on a mid-brightness colored button, e.g.) now have
  mediocre-but-not-broken contrast. This is the honest state of it: a real, working, distinctly
  light theme, not a hand-tuned one.
- **Decorative epoch 1/2/3 imagery (banner photo, footer/home-content GIF textures) is `dark`'s
  actual image files, copied byte-for-byte** into `light/assets/`. They still look like dark-mode
  assets (a neon banner photo, grungy dark textures) sitting on a light page - functional, not
  aesthetically tailored. Swapping them for light-appropriate imagery is future design work, not
  an architecture change - just new files at the same paths.
- **Colors are real, not copied**: `light`'s own `THEME_DEFAULTS` entry (§9.1) is a genuine light
  palette (off-white background, near-black text, a blue accent instead of cyan), so
  `/dashboard/settings/themes` and every epoch's rendering reflect an actual light theme, not
  dark's colors on a white background.

Verified end-to-end: switching to `light` via the navbar (and via a raw `theme` cookie) changes
the stylesheet link, the injected `--br-color-*` CSS variables, and epoch 1/2's substituted
`bgcolor`/`text`/`link`/`vlink` attributes, simultaneously and correctly, across all five epochs
and both the admin panel and public pages.

### 9.4 Recommendation for a future pass: fold epoch -1/0/1/2 into `templates/` too

`light/` needing a full copy of `dark/`'s ~140 files (not just its ~10 epoch-3 ones) to exist at
all is the direct cost of §2's original classification: `menu/`, `mainbanner/`, `layout/`,
`category-menu/`, `home-content/`, `home-blog/` and `page/page-home` were all classified
**theme**-owned for every epoch, including -1/0/1/2, where - as §9.1/§9.2 just demonstrated in
practice - there is now almost nothing left to actually differentiate: colors are unified and
DB-driven regardless of which theme's markup is loaded, and the decorative imagery is
realistically shared/copied rather than bespoke per theme anyway.

**Not executed in this pass** (a ~67-file `git mv`, plus verifying every epoch/route still
renders identically afterward - its own dedicated pass, the same shape as the original
`templates/`/`themes/` split in §7): move epoch -1/0/1/2 variants of those six directories into
`html/templates/`, leaving only their epoch 3 files (plus `styles_epoch3.css` and epoch-3 assets)
theme-owned. A new theme would then need to ship perhaps 10-15 files instead of ~140, with older
epochs automatically inheriting the shared retro shell - colors already come from the theme
regardless via §9.1's table. The one real judgment call this raises: shared epoch<3 files would
hardcode `dark`'s asset paths as everyone's shared imagery (`/themes/dark/assets/...`), which is
already effectively what happened when `light/` copied those exact files - formalizing it as the
deliberate shared default, rather than a per-theme duplicate, is the actual proposal here.

## 10. Figma-sourced 13-variable color model (replaces §5's 7-field `CmsThemeColors`)

> **Status**: implemented. `CmsThemeColors` now carries exactly the 13 variables defined in the
> project's Figma file ("Color palette Dark"/"Color palette Light" collections), replacing the
> old ad-hoc 7-token set (`background`/`text`/`accent`/`author`/`date`/`category`/`border`). One
> merged, non-epoch-split color form still applies (per §9's "no separar por época" requirement) -
> only the token count and names changed, not the "one set of colors for every epoch with a color
> model" architecture.

The 13 fields, their purpose, and their Figma-matching hex defaults:

| Field (`CmsThemeColors`)      | BSON key (hyphenated)        | dark      | light     |
|--------------------------------|-------------------------------|-----------|-----------|
| `navbar_background`            | `navbar-background`           | `#241144` | `#8dd3ff` |
| `navbar_menu_normal`           | `navbar-menu-normal`          | `#ffffff` | `#680072` |
| `navbar_menu_hover`             | `navbar-menu-hover`           | `#98ffdd` | `#006d49` |
| `navbar_menu_active`           | `navbar-menu-active`          | `#00ffab` | `#3800aa` |
| `navbar_logo`                  | `navbar-logo`                 | `#ffffff` | `#680072` |
| `body_background`              | `body-background`             | `#241144` | `#8dd3ff` |
| `home_content_background`      | `home-content-background`     | `#170c29` | `#cbebff` |
| `home_content_text`            | `home-content-text`           | `#ffffff` | `#000000` |
| `blog_list_item_background`    | `blog-list-item-background`   | `#000000` | `#ffffff` |
| `blog_list_item_border`        | `blog-list-item-border`       | `#00ffab` | `#0009ac` |
| `blog_list_item_author`        | `blog-list-item-author`       | `#dfd106` | `#cb5600` |
| `blog_list_item_categories`    | `blog-list-item-categories`   | `#98ffdd` | `#0076c0` |
| `blog_list_item_date`          | `blog-list-item-date`         | `#e68e4e` | `#00636a` |

BSON field names are hyphenated to match the Figma variable names exactly (Mongo/BSON permits
hyphens in field names), rather than being translated to `snake_case`.

**Rendering**: unchanged mechanism from §5's "Rendering" subsection, just re-sourced to 13
tokens instead of 7. Epoch 3 gets all 13 as `--br-color-<name>` CSS custom properties injected
via a `<style>:root{...}</style>` block; every `styles_epoch3.css` rule that previously
referenced a color inline (or an old 7-token `var()`) now reads its matching `--br-color-*`
variable with the theme's own default baked in as the CSS `var()` fallback - so a theme with no DB
override, or a DB document missing a given key, still renders identically to before this change.
Epoch 1/2 (no CSS custom properties there) keep the existing narrower substitution: only
`body_background`/`home_content_text`/`navbar_menu_normal` reach epoch 1/2 markup, via
`{{COLOR_BACKGROUND}}`/`{{COLOR_TEXT}}`/`{{COLOR_ACCENT}}`, unchanged from §9's split - the other
10 variables have no epoch 1/2 equivalent (no matching visual element, or no attribute to carry a
color on those epochs' markup).

**Admin form** (`/dashboard/settings/themes`, `settings-themes-panel_epoch3.html`): 13
`<input type="color">` fields grouped into four `<fieldset>`s (Navbar, Body, Home content, Blog
list item), POSTing to the same `/dashboard/settings/themes/<key>/colors` route, one merged form
per theme as before - no per-epoch split.

**Migration**: the old 7-key `colors` subdocuments in the `themes` collection (`background`,
`text`, `accent`, `author`, `date`, `category`, `border`) were `$unset` from both the `dark` and
`light` documents in production Mongo, rather than translated - `copy_field()` only recognizes
the 13 new hyphenated keys, so leaving the old subdocument in place would have meant an empty-
looking `colors: {}` object was still present while every field silently fell through to
`THEME_DEFAULTS` anyway; unsetting it restores the documented "no `themes` doc / no `colors`
subdocument → hardcoded defaults" fallback guarantee cleanly. Verified end-to-end on a local
`:8080` instance: default rendering for both themes (epoch 1/2/3), the admin form listing and
saving all 13 fields per theme via a real authenticated POST, and the saved values propagating
live into both the epoch 3 `:root` style block and (where applicable) epoch 1/2 attributes.
