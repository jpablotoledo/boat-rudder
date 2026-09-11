#ifndef TEMPLATE_UTILS_H
#define TEMPLATE_UTILS_H

// Returns a new malloc'd string with the first occurrence of `needle` in
// `src` replaced by `replacement`. If `needle` does not occur in `src`, a
// plain copy of `src` is returned. Returns NULL on allocation failure.
char *str_replace_first(const char *src, const char *needle, const char *replacement);

// Renders a printf-style template into a new malloc'd, NUL-terminated
// string. Returns NULL on allocation/formatting failure.
char *render_template(const char *tpl, ...);

// Appends `src` to the malloc'd string `dst` (NULL is treated as an empty
// string), freeing `dst`. Returns the new buffer, or NULL on allocation
// failure (in which case `dst` has already been freed).
char *str_append(char *dst, const char *src);

// Builds a "<title>page_title</title>" tag as a new malloc'd string, for
// substitution into a template's {{PAGE_TITLE}} placeholder. Returns NULL on
// allocation failure.
char *build_title_tag(const char *page_title);

// Returns a new malloc'd URL with `suffix` inserted before the file extension.
// e.g. image_url_variant("/img/photo.jpg", "_small") → "/img/photo_small.jpg"
// Returns a copy of the original if no extension is found. NULL on failure.
char *image_url_variant(const char *url, const char *suffix);

// Returns a new malloc'd URL slug from `name`: lowercased, spaces/hyphens/
// underscores collapsed to a single hyphen, other non-alphanumeric characters
// removed. e.g. "Operating Systems" -> "operating-systems". NULL on failure.
char *slugify(const char *name);

// Returns a new malloc'd copy of `s` with its first byte uppercased if it is
// a lowercase ASCII letter - every theme key in this codebase is a plain
// lowercase slug ("dark", "light"), so this is all that's needed to display
// it as "Dark"/"Light" on epoch 1/2 (no stylesheet there for a
// text-transform: capitalize the way epoch 3 gets it - see
// styles_epoch3.css). NULL on failure.
char *capitalize_first(const char *s);

#endif // TEMPLATE_UTILS_H
