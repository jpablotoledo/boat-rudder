# Theme-Scoped Banner/Footer - Refactoring Plan

> **Status**: implemented as described in §3-§7. §3.1's epoch-index move landed as
> `EPOCH_COUNT`/`epoch_to_index()` in `detect_epoch.h`. §4's asset-upload fix shipped as part of
> the same change - `theme_assets_dir()` now takes an explicit, validated `key`, never
> `request_theme()`; verified live that uploading to `light`'s assets while the admin's own
> session resolved to `dark` lands in `light`'s directory, not `dark`'s. §8's migration used the
> one-shot path in spirit (a direct one-time copy of the pre-existing
> `site_settings.footer_html` into `themes.dark.footer_html`, then unset the old fields - no
> `banner_html` existed yet to migrate) rather than building an "Import" admin action for what
> turned out to be a single real document. §9.1 resolved in favor of dropping `CmsSiteSettings`
> entirely - `cms_site_settings.c` is now two independent accessor pairs (site name, active
> theme), no wrapping struct. §9.2 (a shortcut from the general settings page to the active
> theme's editors) not done. Verified end-to-end: both themes' banners/footers save and render
> independently, every route across both themes returns 200, and a full clean rebuild has no
> warnings.
>
> Written after
> [theme-system-plan.md](theme-system-plan.md) shipped a real second theme (`light`) and exposed
> the gap this plan fixes: colors are per-theme (`themes.<key>.colors`), but the home banner and
> footer are still site-wide (`site_settings.banner_html`/`footer_html`, from
> [site-personalization-plan.md](site-personalization-plan.md)) - so switching themes changes
> the palette but not the banner/footer markup that palette was picked to go with. This plan
> moves banner/footer into the `themes` collection, alongside colors, and reshapes
> `/dashboard/settings` so a theme's colors, banner and footer are edited together as one thing.

## 1. Goal

Today, "Site settings" is flat: site name, Banner, Footer, Preview and Themes are five
independent top-level links, and Banner/Footer apply to *every* theme identically regardless of
which one is active. That was correct when there was exactly one theme (banner/footer had
nowhere else to belong), but it stopped being correct the moment a second theme existed: a
banner written with `dark`'s neon palette in mind (raw HTML, hand-styled per epoch - see
site-personalization-plan.md §1) doesn't necessarily read as well against `light`'s off-white
background, and there is no way to give it a different one.

The fix is conceptually simple - move `banner_html`/`footer_html` from `site_settings` (singleton,
site-wide) to `themes.<key>` (one per theme, alongside `colors`) - but touches the DB schema, the
render path, the admin routes, and - the part worth planning carefully - the theme-asset upload
endpoints, which have their own latent bug once banner/footer become per-theme (§4).

## 2. Current state

```jsonc
// db.site_settings - singleton
{ site_name, active_theme, banner_html: {epoch_neg1..epoch3}, footer_html: {epoch_neg1..epoch3} }

// db.themes - one per theme
{ key, colors: {accent, background, text, author, date, category, border} }
```

- `mainbanner()` (`src/modules/mainbanner/mainbanner.c`) calls `cms_get_site_banner(epoch)`
  (`src/db/cms_site_settings.c`): the DB value if non-empty, otherwise a file-fallback read via
  `generate_url_theme("mainbanner/mainbanner_epoch%d.html", epoch)` - and *that* fallback is
  already theme-aware (`generate_url_theme()` resolves against `request_theme()`). So today's
  behavior is a real inconsistency, not just a missed feature: an **unedited** banner already
  differs by theme (each theme's own on-disk file), but the moment an admin **saves** a custom
  banner, it becomes the same for every theme. Same story for the footer via
  `page_layout_wrap()`'s `splice_footer()`/`cms_get_site_footer()`.
- Admin UI: `/dashboard/settings` (site name + links), `/dashboard/settings/banner` and
  `/dashboard/settings/footer` (5 epoch textareas + asset upload each, both theme-agnostic),
  `/dashboard/settings/preview`, `/dashboard/settings/themes` (per-theme colors + activate).
- Theme-asset uploads (`/dashboard/api/theme-assets/{list,upload,delete}`,
  `theme_assets_dir()` in `http_router.c`) write to
  `html/themes/<request_theme()>/assets/<mainbanner|footer>/epoch<N>/` - i.e. **whichever theme
  is active for the admin's own browsing session**, not necessarily the theme whose banner
  editor is open. This is harmless today only because there is one banner/footer for every
  theme to share; §4 covers why it stops being harmless here.

## 3. Proposed schema: banner/footer move into `themes.<key>`

```jsonc
// db.themes - one per theme, now three sibling fields instead of one
{
  "_id": ObjectId,
  "key": "dark",
  "colors": { "accent": "#56e9fd", "background": "#000000", "text": "#ffffff",
              "author": "#f2e200", "date": "#f9964f", "category": "#349b00", "border": "#349b00" },
  "banner_html": { "epoch_neg1": "", "epoch0": "", "epoch1": "", "epoch2": "", "epoch3": "" },
  "footer_html":  { "epoch_neg1": "", "epoch0": "", "epoch1": "", "epoch2": "", "epoch3": "" }
}

// db.site_settings - shrinks to what's genuinely site-wide, not per-theme
{
  "_id": ObjectId,
  "site_name": "Boat Rudder",
  "active_theme": "dark"
}
```

"" still means "fall back to the on-disk theme file" - same rule as today, just resolved per
theme document instead of a single shared one. Nothing about the *file* fallback changes: each
theme already has its own `mainbanner/mainbanner_epoch<N>.html` and `layout/footer_epoch<N>.html`
(theme-owned per theme-system-plan.md §2), so a theme with no saved banner/footer keeps rendering
its own on-disk default exactly as before this plan.

### 3.1 Shared epoch-index helper moves out of `cms_site_settings.h`

`SITE_SETTINGS_EPOCH_COUNT`/`cms_site_settings_epoch_index()` exist to map epoch (-1..3) to a
0..4 array index for `banner_html`/`footer_html`. Once those fields leave `site_settings`, the
helper has no reason to live there - it's a property of "the five epochs," not of site settings
specifically, and `cms_themes.c` becomes its only real consumer alongside the admin module. Move
it to `src/utils/detect_epoch.h` as `EPOCH_COUNT`/`epoch_to_index()`, generically named, so
either `cms_site_settings.c` (if it ever needs it again) or `cms_themes.c` can use it without one
depending on the other.

## 4. The asset-upload bug this plan would otherwise ship

`theme_assets_dir()` resolves the target directory from `request_theme()` - the theme active for
*this request* (query > cookie > site default, per theme-system-plan.md §9.2). That is exactly
right for a **visitor** viewing the site, and exactly wrong for an **admin editing a specific
theme's banner**: nothing stops an admin whose own browsing session is under the `light` cookie
from opening `/dashboard/settings/themes/dark/banner` and uploading an image - which would land
in `light`'s asset directory, not `dark`'s, silently.

**Fix**: the banner/footer editor pages and their asset endpoints take an explicit theme key from
the URL/query string, never from `request_theme()`. `theme_assets_dir()` gains a `key` parameter
(validated with `theme_key_is_valid()`, same guard already used for the cookie/query/admin-form
paths per theme-system-plan.md §9.2) instead of reading the active theme implicitly. This is a
small, mechanical change, but worth calling out as its own step (§7) since it's easy to miss -
the bug is latent today (one banner for every theme, so "which theme" never mattered) and only
becomes real the moment this plan ships.

## 5. Proposed DB layer: extend `src/db/cms_themes.h/.c`

```c
// db.themes.findOne({key}). Returns a malloc'd string: the DB value for
// `epoch` if non-empty, otherwise the on-disk fallback
// (mainbanner/mainbanner_epoch<N>.html / layout/footer_epoch<N>.html for
// that *same* theme, via generate_url_theme()'s normal resolution). Never
// NULL. Used by mainbanner() and page_layout_wrap() with
// key = request_theme() - the visitor's own resolved theme, so a banner
// follows the same per-visitor precedence as colors already do.
char *cms_get_theme_banner(const char *key, int epoch);
char *cms_get_theme_footer(const char *key, int epoch);

// The *stored* value only ("" if unset - not file-resolved), one per
// epoch, for the admin form: it needs to tell "nothing saved" apart from
// "saved text that happens to equal the file," same distinction
// site-personalization-plan.md's cms_get_site_settings() already drew.
// out_values is caller-allocated with EPOCH_COUNT entries, each malloc'd -
// caller frees every entry.
void cms_get_theme_banner_values(const char *key, char *out_values[EPOCH_COUNT]);
void cms_get_theme_footer_values(const char *key, char *out_values[EPOCH_COUNT]);

// db.themes.updateOne({key}, {$set: {"banner_html.<field for epoch>": html}},
// {upsert: true}). Returns 0 on success, -1 if epoch is outside -1..3, on
// a DB error, or if mongodb is not ready.
int cms_update_theme_banner(const char *key, int epoch, const char *html);
int cms_update_theme_footer(const char *key, int epoch, const char *html);
```

`CmsThemeColors` (colors only) stays as-is - banner/footer values don't need a struct the way
colors do, since the admin form already works off a plain `char *[EPOCH_COUNT]` array (mirroring
`site_settings_banner_page()`'s existing shape almost exactly, just re-keyed by theme).

### 5.1 What leaves `cms_site_settings.h/.c`

`CmsSiteSettings.banner_html`/`.footer_html`, `cms_get_site_banner()`/`cms_get_site_footer()`,
`cms_update_site_banner()`/`cms_update_site_footer()`, `cms_site_settings_epoch_index()` (moved,
§3.1) - all removed. `cms_get_site_settings()` shrinks to just `site_name` (the struct becomes
almost trivial - worth asking, while touching this, whether `CmsSiteSettings` should keep being a
struct/`cms_get_site_settings()` pair at all versus folding into a single
`cms_get_site_name()`-shaped accessor; left as an open question, §9.1, not a blocker).

## 6. Rendering changes

- **`mainbanner()`** (`src/modules/mainbanner/mainbanner.c`): `cms_get_theme_banner(request_theme(), epoch)`
  instead of `cms_get_site_banner(epoch)`. One-line change, same NULL/ownership contract.
- **Footer** (`src/html_builder/page_layout.c`, `splice_footer()`): `cms_get_theme_footer(request_theme(), epoch)`
  instead of `cms_get_site_footer(epoch)`. Same shared-tail reasoning as before - one change site
  covers home, page, entry, blog listing, dashboard, login, every error page.

Both now correctly follow whichever theme is active *for that request* - including a visitor's
own `theme` cookie override, exactly like colors already do. A visitor who has chosen `light`
sees `light`'s banner and footer, not `dark`'s, the same way they already see `light`'s colors.

## 7. Proposed admin UI: banner/footer move under Themes

- `/dashboard/settings` (general) keeps the site name form, and now links to **Themes** and
  **Preview** only - Banner/Footer are no longer reachable as theme-agnostic top-level pages,
  because they no longer are theme-agnostic.
- `/dashboard/settings/themes` (list): each theme's panel gains two links alongside its existing
  colors form and "Set active" button - **"Edit banner"** and **"Edit footer"**, to
  `/dashboard/settings/themes/<key>/banner` and `/dashboard/settings/themes/<key>/footer`.
- `/dashboard/settings/themes/<key>/banner` and `.../footer` (new): the *same* UI
  site-personalization-plan.md §7 already built (`site_settings_banner_page()`/
  `site_settings_footer_page()`, `settings-asset_epoch3.html`/`settings-asset-panel_epoch3.html`)
  - one `<textarea>` + asset upload/browse widget per epoch - now parametrized by `key` (from the
  URL) instead of implicitly meaning "the site's one banner." The asset browser's upload/list/
  delete calls pass `&theme=<key>` explicitly (§4), not `request_theme()`.
- Routes (admin-gated, form POST + redirect, same convention as every other settings sub-page):

  | Route | Method | Behavior |
  |---|---|---|
  | `GET /dashboard/settings/themes/<key>/banner` | GET | 404/redirect if `key` isn't a real theme; otherwise `cms_get_theme_banner_values(key, …)` + the asset-page UI. |
  | `POST /dashboard/settings/themes/<key>/banner/<epoch>` | POST | `cms_update_theme_banner(key, epoch, html)`, redirect back to the same editor. |
  | `GET /dashboard/settings/themes/<key>/footer` | GET | Mirrors banner. |
  | `POST /dashboard/settings/themes/<key>/footer/<epoch>` | POST | Mirrors banner. |
  | `GET /dashboard/api/theme-assets/list?theme=<key>&component=…&epoch=…` | GET | `theme_assets_dir()` takes `key` from the query string, not `request_theme()` (§4). |
  | `POST /dashboard/api/theme-assets/upload?theme=<key>&component=…&epoch=…` | POST | Same. |
  | `POST /dashboard/api/theme-assets/delete?theme=<key>&component=…&epoch=…&file=…` | POST | Same. |

- The two-segment route (`<key>` then `<epoch>`) needs its own matcher - `http_router.c` already
  has this exact shape for `/dashboard/api/entries/<entry_id>/blocks/<block_id>/delete`
  (`match_block_delete_route()`); a `match_theme_epoch_route()` mirrors it for
  `/dashboard/settings/themes/<key>/banner/<epoch>` and `.../footer/<epoch>`.
- `site_settings_admin.c`'s `asset_page()` (the shared helper behind both banner and footer pages
  today) gains a `key` parameter it threads into `cms_get_theme_banner_values()`/
  `cms_update_theme_banner()` and into the asset-browser's data attributes (so the page's own
  JS - already written to read `data-component`/`data-epoch` off each panel - also carries
  `data-theme`, appended to its three fetch calls).

## 8. Migration

One singleton `site_settings.banner_html`/`footer_html` needs to become one theme's
`themes.dark.banner_html`/`footer_html` (`dark` because it was the only theme when that content
was written). Two ways to do it, not mutually exclusive:

1. **A one-shot admin action** on `/dashboard/settings/themes/dark/banner` (and `/footer`):
   "Import from site settings," reading the old `site_settings` fields once and writing them into
   `themes.dark`. Mirrors the "Import current theme files" idea site-personalization-plan.md §8
   step 7 already floated for a different migration (disk -> DB) - same shape, one field group
   at a time, explicit and reviewable rather than automatic.
2. **Manual re-entry**: given banner/footer content is short, hand-authored HTML (a handful of
   epochs' worth, not a bulk dataset), an admin could simply re-paste it into the new per-theme
   form. Realistic for a single-admin, pre-scale install like this one; less realistic once
   multiple themes/admins are actually in play.

Either way, the old `site_settings.banner_html`/`footer_html` fields are left in the document
(dead, unread) rather than actively deleted - matches this codebase's general pattern of never
needing a hard migration step for a schema shrink, since unread fields are harmless and the
getters simply stop looking for them.

## 9. Open questions

1. **Does `CmsSiteSettings` still earn its keep** once it's down to just `site_name` (`colors`
   already left in the original theme-system-plan.md, `active_theme` lives beside it via its own
   accessor pair already, and now banner/footer leave too)? Possibly collapses to a single
   `cms_get_site_name()`/`cms_update_site_name()` pair with no wrapping struct at all. Not a
   blocker either way - worth deciding once this plan's removals are actually written, not
   before.
2. **Should the general `/dashboard/settings` page keep a shortcut to the *active* theme's
   banner/footer editors**, alongside the full theme list - "you're probably here to edit what
   you're currently looking at"? A small UX nicety, not required for correctness; the full
   `/dashboard/settings/themes` list always has it too.
3. **Precedence confirmed as `request_theme()`** (§6) - i.e., a visitor's own theme cookie
   determines which theme's banner/footer they see, exactly like colors. Flagged explicitly in
   case that reads as surprising: it means two visitors on the same site, one preferring `light`
   and one `dark`, can genuinely see different banners, not just different colors.
4. **The migration step (§8)**: pick one-shot admin action vs. manual re-entry before
   implementation - both are cheap, but only one should actually get built.
