# Site Personalization (name, banner, footer) - Implementation Plan

> **Status**: implemented. `site_name`, `banner_html`/`footer_html` (§3-§7), the theme-asset
> upload endpoints (§6), `/dashboard/settings/preview` and the navbar's signed-in-user link all
> shipped as described below (some minor deltas from later, smaller follow-up requests - e.g. the
> user link ended up in the navbar's left corner + the mobile menu, not exactly where §7
> originally imagined - not worth rewriting the plan for). This document supersedes/extends
> [site-settings-plan.md](site-settings-plan.md) (roadmap item, [boat-rudder.md §9](../boat-rudder.md)):
> it keeps that plan's `site_name` design as-is and adds the two pieces requested next -
> a raw-HTML **home banner** editor and a raw-HTML **footer** editor, one field per epoch,
> plus image uploads for the assets they reference. `site-settings-plan.md`'s favicon/logo/colors
> scope is unchanged and not repeated here.
>
> **Relationship to the theme system:** [theme-system-plan.md](theme-system-plan.md) splits
> `html/themes/<theme>/` into a shared `html/templates/` tree plus a slimmer, brand-only
> `html/themes/<theme>/` tree; at that point `site_settings.banner_html`/`footer_html` were left
> as designed here (site *content*, not theme *design*), unaffected by the split. That stopped
> being the right call once a second theme (`light`) actually existed - see
> [theme-scoped-personalization-plan.md](theme-scoped-personalization-plan.md), which **moves
> `banner_html`/`footer_html` from `site_settings` into `themes.<key>`**, alongside colors: a
> banner is part of a theme's look, not the site's identity, the same distinction that plan draws
> for colors. `cms_get_site_banner()`/`cms_get_site_footer()` and their `/dashboard/settings/
> banner`/`/footer` routes described in §3-§7 below are superseded by that plan once implemented.

Diagrams: reuse [diagrams/site-settings-components.puml](../diagrams/site-settings-components.puml)
as a base; extend it once implementation starts.

## 1. Goal

A single **"Personalización" / "Site settings"** section under `/dashboard`, admin-only
(`role == "admin"`, same gate as Categories/Languages/Menu/Users), covering:

1. **Site name** - as already specced in [site-settings-plan.md §3](site-settings-plan.md) (a
   plain text field, propagated via `%s` into ~16 templates). No changes proposed here.
2. **Home banner** - today `html/themes/dark/mainbanner/mainbanner_epoch{-1,0,1,2,3}.html`, five static
   files. Goal: move their content into MongoDB, editable as raw markup from a `<textarea>` per
   epoch, with a fallback to the on-disk file when the DB value is empty - so a fresh clone
   renders identically to today until an admin opens the editor.
3. **Footer** - the same treatment for `html/themes/dark/layout/footer_epoch{-1,0,1,2,3}.html`.
4. **Image uploads** for the assets those two blocks reference, written into the same
   theme-asset directories used today (`html/themes/<theme>/assets/mainbanner/epoch<N>/`,
   plus a new `assets/footer/epoch<N>/`), separate from the content media library
   (`/dashboard/media`, which is scoped to blog/page content under `html/content/posts/...`).

This is deliberately **raw markup, not a WYSIWYG banner builder**: epoch 1/2 banners are
decorative `<table>` layouts and epoch -1's is a WML fragment, not HTML at all - a structured
"banner settings" form (single image + caption) cannot express what these epochs need. A
textarea matches the project's existing precedent: the entry editor's `generic` content block
type already stores raw HTML for epoch 3 with no sanitization, on the same trust level (admin/
author-authored, not visitor input). See §7 for why that precedent extends cleanly here.

## 2. Current state

| Epoch | Banner (`mainbanner_epoch<N>.html`) | Footer (`footer_epoch<N>.html`) |
|---|---|---|
| -1 (WML) | `<p>` + one `<img>` (`.wbmp`) | empty |
| 0 | empty | `<hr><p>Boat Rudder</p>` |
| 1 | decorative `<table>` with one `<img>` | `<table>` with one `<img>` |
| 2 | decorative `<table>`, two `<img>` | `<table>`, two `<img>` |
| 3 | `<div class="boat-rudder__mainbanner"><img ...></div>` | `<footer>...Boat Rudder...</footer>` |

Both are read from disk on every request:

- **Banner**: `mainbanner()` (`src/modules/mainbanner/mainbanner.c`) does
  `generate_url_theme("mainbanner/mainbanner_epoch%d.html", epoch)` + `read_file_to_string()`, no
  placeholders. The result is spliced into `container_epoch<N>.html` as a `%s` **argument**
  (not a format string), so any literal `%` inside it already passes through `vsnprintf`
  unchanged - important for §5.
- **Footer**: not a module - `page_layout_wrap()` (`src/html_builder/page_layout.c`) calls a
  private `splice_part()` that reads `layout/footer_epoch<N>.html` from disk and substitutes it
  into the fragment's `{{FOOTER}}` marker via `str_replace_first()` (plain text substitution,
  not `printf`). `page_layout_wrap()` is the shared tail of **every** page type (home, generic
  page, entry, blog listing), so a DB-backed footer only needs one change site.

Image assets referenced by the current five banner files live at:
```
html/themes/dark/assets/mainbanner/
├── epoch-1/mobile-m.png, mobile-m.wbmp
├── epoch1/main.gif
├── epoch2/background.gif, sea.gif, background-wb.gif
└── epoch3/main.jpg
```
(no `epoch0/` - epoch 0's banner is empty today). Footer assets live at
`html/themes/dark/assets/footer/` (referenced by `footer_epoch{1,2}.html`, not yet listed above).

## 3. Proposed schema: extend `site_settings` (singleton, from site-settings-plan.md)

Same collection, two new sub-documents. Field names spell out the epoch to avoid a leading
`-` in a BSON key:

```jsonc
// db.site_settings - exactly one document
{
  "_id": ObjectId,
  "site_name": "Boat Rudder",        // from site-settings-plan.md, unchanged
  // ... favicon_url / logo_url / colors from site-settings-plan.md, unchanged ...

  "banner_html": {
    "epoch_neg1": "",   // "" = fall back to mainbanner/mainbanner_epoch-1.html on disk
    "epoch0":     "",
    "epoch1":     "",
    "epoch2":     "",
    "epoch3":     ""
  },
  "footer_html": {
    "epoch_neg1": "",
    "epoch0":     "",
    "epoch1":     "",
    "epoch2":     "",
    "epoch3":     ""
  }
}
```

**Empty string means "use the on-disk theme default"** - the same never-break-a-fresh-install
guarantee used throughout the codebase (`menu()`'s `FALLBACK_ITEMS`, and site-settings-plan.md's
own fallback rule). This also means an admin can revert to the theme default per-epoch just by
clearing the textarea, without redeploying.

## 4. Proposed DB layer: extend `src/db/cms_site_settings.h/.c`

`cms_site_settings.c` does not exist yet (site-settings-plan.md is unimplemented) - this plan
assumes it is created together with these additions, not as a later patch.

```c
// Index convention: array index i corresponds to epoch (i - 1), i.e.
// index 0 = epoch -1, index 1 = epoch 0, ..., index 4 = epoch 3.
#define SITE_SETTINGS_EPOCH_COUNT 5

typedef struct {
    char site_name[128];
    // ... favicon_url / logo_url / colors, per site-settings-plan.md ...

    // malloc'd, arbitrary length (unlike the fixed char[] fields above -
    // these hold raw markup, not short admin-entered strings). "" (not
    // NULL) when unset, so callers never need a NULL check before strlen().
    char *banner_html[SITE_SETTINGS_EPOCH_COUNT];
    char *footer_html[SITE_SETTINGS_EPOCH_COUNT];
} CmsSiteSettings;

// Maps epoch (-1..3) to a banner_html/footer_html array index (0..4), or -1
// if out of range.
int cms_site_settings_epoch_index(int epoch);

// db.site_settings.findOne({}). Fills `out`, including banner_html/footer_html
// (each "" if unset or the document/DB is unreachable). Always returns 1 -
// personalization must never fail the page. Caller must call
// cms_site_settings_free(out) when done.
int cms_get_site_settings(CmsSiteSettings *out);

void cms_site_settings_free(CmsSiteSettings *settings);

// $set site_settings.banner_html.<field for epoch>, upsert. field is one of
// the 5 keys in §3. Returns 0 on success, -1 on an invalid epoch, a DB
// error, or if mongodb is not ready.
int cms_update_site_banner(int epoch, const char *html);
int cms_update_site_footer(int epoch, const char *html);

// Existing site-settings-plan.md function, unchanged:
int cms_update_site_settings(const CmsSiteSettings *settings);
```

Two narrower helpers avoid loading the whole singleton on the hot path (`mainbanner()` runs on
every `GET /`):

```c
// Returns a malloc'd string: the DB value for that epoch if non-empty,
// otherwise the on-disk theme file's contents (same fallback the caller
// would otherwise have to implement itself). Never returns NULL - on a
// missing file *and* an empty DB value, returns a malloc'd "".
char *cms_get_site_banner(int epoch);
char *cms_get_site_footer(int epoch);
```

`cms_get_site_banner`/`cms_get_site_footer` are what `mainbanner()` and `page_layout_wrap()` actually
call; `cms_get_site_settings()` (whole-document read) stays scoped to the `/dashboard/settings*`
admin pages, which need every field at once for the forms.

## 5. Rendering changes

- **`mainbanner()`** (`src/modules/mainbanner/mainbanner.c`): replace the direct
  `generate_url_theme()` + `read_file_to_string()` call with
  `cms_get_site_banner(epoch)`. No template placeholder changes - the container still receives
  the result as one opaque `%s` argument, so this is a one-function change with no cascading
  edits to `container_epoch<N>.html`.
- **Footer** (`src/html_builder/page_layout.c`): `splice_part()`'s footer case switches from
  reading `layout/footer_epoch<N>.html` off disk to `cms_get_site_footer(epoch)`. Because
  `page_layout_wrap()` is the single shared tail for every page type, this one change covers the
  home page, `/page/<link>`, `/blog/<link>`, `/blog`, `/dashboard`, `/login` and every error
  page - matching how `{{FOOTER}}` already works today.
- Both changes are **drop-in**: signatures of `mainbanner()` and `page_layout_wrap()` don't change,
  so no caller elsewhere in the codebase needs touching.

### 5.1 Why raw admin HTML is safe to interpolate here

Two independent reasons this doesn't reopen the format-string concern in
[boat-rudder.md §2.4](../boat-rudder.md#24-two-kinds-of-placeholders):

- The banner is substituted as a **`%s` argument value** into `container_epoch<N>.html`, not as
  a format string itself - `vsnprintf` does not re-interpret `%` characters inside an argument
  (documented already, §2.4). No escaping is required or should be applied to admin-entered
  banner text.
- The footer is substituted via `str_replace_first()` (`{{FOOTER}}` marker), which is plain text
  replacement, not `printf`-family - `%` has no special meaning there at all.

So no new escaping logic is needed in the rendering path. The one thing to verify during
migration (§8, step 2) is `mainbanner_epoch1.html`'s existing `100%%` - confirm whether that's a
pre-existing double-escaping quirk that should become a single `%` once the content moves off a
file that's read verbatim and into a DB value substituted as a `%s` argument (it should - a
`%%` literal would render as a literal `%%` in the browser once it's no longer passed through
`printf` at all).

## 6. Image asset uploads (banner and footer)

A dedicated, minimal upload surface scoped to **theme assets** - not the content media library.
Rationale: `/dashboard/media` (`cms_media` collection) is modeled around per-author content
directories under `html/content/posts/<username>/...` with galleries, pagination and an image
picker built for entry content. Theme assets are a handful of curated files per epoch, owned by
the theme, not by an author - reusing the content media library would mean either polluting it
with theme files or building a parallel directory concept inside it. A small, separate endpoint
is less code than bending that system to fit.

- **Storage**: `html/themes/<theme>/assets/<component>/epoch<N>/<sanitized-filename>`, where
  `<component>` is `mainbanner` or `footer` and `<N>` is the epoch suffix already used on disk
  (`-1`, `0`, `1`, `2`, `3` - `epoch0/` is created on first upload, it doesn't exist yet).
  Served automatically by the existing static file server (`/themes/dark/assets/...`), no new
  route needed for *reading* the files.
- **Filename sanitization**: reuse whatever `cms_media`'s upload path already applies (documented
  as "sanitized filename, no spaces, alphanumeric/-/.") rather than writing a second sanitizer.
- **New routes** (admin-only, `require_dashboard_session_role(..., "admin")`):

  | Route | Method | Behavior |
  |---|---|---|
  | `GET /dashboard/api/theme-assets/list?component=mainbanner&epoch=3` | GET | `readdir()` the target directory, return `{"files":["main.jpg", ...]}` (JSON) so the banner/footer editor can show what's already there and let the admin copy a path into the textarea. |
  | `POST /dashboard/api/theme-assets/upload?component=mainbanner&epoch=3` | POST | Multipart upload (reuse `parse_multipart()` from the media upload handler), write the file, return `{"ok":true,"path":"/themes/dark/assets/mainbanner/epoch3/<name>"}`. |
  | `POST /dashboard/api/theme-assets/delete?component=mainbanner&epoch=3&file=<name>` | POST | `unlink()` after re-sanitizing `file` (defend against `../`, same as `sanitize_path()` elsewhere). |

- **No image-optimizer variants**: unlike content-library uploads, these are hand-picked,
  low-cardinality theme assets (one banner image per epoch, not a growing photo library) -
  running `scripts/image-optimizer.sh` on them would add `_full/_half/_small/...` variants the
  admin then has to pick between with no clear default. Upload the file as given. Flagged as an
  **open question** (§9.3) in case the team disagrees for epoch 1/2's GIF-heavy assets.
- **The textarea stays "raw code," on purpose**: uploading returns a path; the admin pastes that
  path into an `<img src="...">` inside the textarea themselves. No auto-injection into the
  markup - matches the user's own framing ("un textarea... y poder subir imágenes a un
  directorio específico" describes two separate actions) and avoids guessing where in
  hand-authored markup an image tag belongs.

## 7. Proposed admin UI: `/dashboard/settings/banner` and `/dashboard/settings/footer`

Both epoch3-only (dashboard is epoch3-only entirely), admin-only, following the single-document
form shape from site-settings-plan.md §6 rather than the list-of-rows shape used by
Categories/Menu/Users.

- New module `src/modules/site_personalization_admin/` (or extend the `settings_admin` module
  site-settings-plan.md already proposes, adding two more entry points to the same module -
  recommended, since all three views share one "site settings" nav link and one admin-role
  gate):
  ```c
  char *settings_banner_page(int epoch, char *const banner_html[SITE_SETTINGS_EPOCH_COUNT]);
  char *settings_footer_page(int epoch, char *const footer_html[SITE_SETTINGS_EPOCH_COUNT]);
  ```
  Each renders one `<textarea>` per epoch (5 total) pre-filled with the current DB value (or, if
  empty, optionally pre-filled with the *current on-disk file's* contents rather than a blank
  box - see §9.2 - so the admin edits from a known starting point instead of an empty textarea
  that would otherwise silently mean "delete the banner").
- New templates `html/themes/dark/dashboard/settings/settings-banner_epoch3.html` and
  `settings-footer_epoch3.html`, each with a small file-list + upload widget (§6) above or beside
  every textarea, labeled per epoch (e.g. "Epoch -1 (WAP/WML)", "Epoch 3 (Modern)") so the admin
  knows epoch -1 expects WML, not HTML.
- **Routes** (form POST + redirect per epoch, not AJAX - consistent with Categories/Menu, not the
  entry editor's AJAX pattern, since each epoch's textarea is a small independent save unit):
  - `GET /dashboard/settings/banner` - admin-role guard -> `settings_banner_page()`.
  - `POST /dashboard/settings/banner/<epoch>` - guard, `cms_update_site_banner(epoch, html)`,
    redirect back to `/dashboard/settings/banner`.
  - `GET /dashboard/settings/footer` / `POST /dashboard/settings/footer/<epoch>` - mirror.
- `/dashboard` gains "Site settings" (or a Spanish-language label if the site content language
  drives dashboard UI text - check current convention in `dashboard/nav-admin_epoch3.html`)
  linking to `/dashboard/settings`, which itself links to the banner/footer sub-pages, per
  site-settings-plan.md §6.

### 7.1 Why no sanitization, and who can reach this

Same trust boundary as the entry editor's `generic` HTML block and rich-text `paragraph` blocks:
this is server-rendered admin-authored content, not visitor input, and the codebase's existing
convention (per `cms-entry-model-plan.md` and the entry editor) is no HTML sanitization on that
class of field. Scoping banner/footer edits to `role == "admin"` (stricter than entries, which
Autor can also edit) keeps the blast radius of a compromised or careless account to the smallest
group already trusted with Users/Menu/Categories.

## 8. Implementation order

1. **DB layer**: `cms_site_settings.h/.c` (create - doesn't exist yet), including
   `banner_html`/`footer_html`, `cms_get_site_banner()`/`cms_get_site_footer()` with
   file-fallback, `cms_update_site_banner()`/`cms_update_site_footer()`. Unit-verifiable in
   isolation (no template/route dependencies yet).
2. **Rendering consumers**: `mainbanner()` and `page_layout_wrap()`'s footer splice switch to the new
   DB-backed getters. Verify every epoch (`force_epoch` in `configs/settings.conf` to check each
   without needing five real browsers) renders **identically to today** with an empty
   `site_settings` document - this is the regression gate before anything else proceeds. Resolve
   the `mainbanner_epoch1.html` `%%` question from §5.1 here.
3. **Theme-asset upload endpoints** (§6): list/upload/delete, admin-gated, reusing
   `parse_multipart()` and the media module's filename sanitizer.
4. **Admin templates + module** (§7): banner and footer forms, wired into whichever module ends
   up owning `/dashboard/settings` (this plan recommends folding into site-settings-plan.md's
   `settings_admin`, built together rather than sequentially, since they share the nav link and
   guard logic).
5. **Routes** in `http_router.c`: the 4 GET/POST view routes + 3 asset routes, all behind
   `require_dashboard_session_role(..., "admin")`.
6. **`/dashboard` nav link** (shared with site-settings-plan.md's own step).
7. **Migration convenience** (optional but recommended for adopting this on the *current*
   deployment, not just fresh clones): a "Import current theme files" action on
   `/dashboard/settings/banner` and `/footer` that reads the 5 on-disk files for that component
   and writes them into the DB as a starting point - equivalent to what §9.2 proposes doing
   automatically at form-render time, but explicit and one-shot instead of implicit on every
   page load.
8. **Docs**: update `dashboard.md` (new maintainer section, mirroring Categories/Menu), update
   `boat-rudder.md §9` (mark this increment in progress / done), fold this plan's outcome back
   into `site-settings-plan.md`'s status line once both ship.
9. **End-to-end verification**: for both banner and footer, per epoch - confirm (a) empty DB ->
   today's exact output, (b) a saved textarea value overrides it, (c) clearing the textarea
   reverts to the on-disk default without a restart, (d) an uploaded image's path is reachable at
   the expected `/themes/<theme>/assets/...` URL, (e) `../`-style paths are rejected by the
   delete endpoint.

## 9. Open questions

1. **Module boundary**: fold banner/footer into site-settings-plan.md's `settings_admin` module
   (one nav entry, shared guard, built together) vs. a separate `site_personalization_admin`
   module. This plan recommends folding in - the two plans are the same feature request answered
   in two increments, and a visitor sees "Site settings" as one thing.
2. **Textarea pre-fill when DB value is empty**: show a blank textarea (matches the "" = use
   on-disk default" semantics literally, but risks an admin thinking the box is broken/empty) vs.
   pre-filling with the current on-disk file's contents as a starting point (friendlier, but
   means "save with no changes" now stores a DB copy identical to the file, silently opting that
   epoch out of future on-disk-default updates if the theme is later upgraded). Recommend:
   pre-fill for editing convenience, but show it in a visually distinct "showing on-disk default -
   not yet saved to DB" state until the admin actually saves.
3. **Image-optimizer variants for theme assets**: skip (§6, this plan's default) vs. reuse
   `scripts/image-optimizer.sh` for consistency with the content media library. Leaning skip,
   given the small, curated nature of these assets, but worth confirming - epoch 1/2's GIF assets
   in particular already went through manual palette/dithering choices that an automated
   optimizer pass could disturb.
4. **WML validity for epoch -1**: the banner/footer editor doesn't validate that epoch -1's
   textarea contains valid WML (no HTML sanitization at all, per §7.1) - an admin who pastes HTML
   into that box will break WAP rendering with no server-side warning. Worth a client-side hint
   only ("this epoch expects WML, not HTML"), not blocking validation - consistent with the
   no-validation convention elsewhere.
5. **Relationship to `site-settings-plan.md`**: should that document be edited in place to fold
   this scope in, or left standalone with a cross-reference (as done here)? Recommend merging
   once both are implemented, to avoid two overlapping "site settings" plan documents long-term.
