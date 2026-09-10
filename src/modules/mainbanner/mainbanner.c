#include "mainbanner.h"
#include "../../db/cms_themes.h"
#include "../../utils/request_theme.h"

char *mainbanner(int epoch) {
    // cms_get_theme_banner() falls back to the active theme's own on-disk
    // mainbanner/mainbanner_epoch<N>.html when the DB has no override, so
    // a fresh install renders exactly as before this became DB-backed -
    // and switching themes (request_theme(), per-visitor) switches the
    // banner along with the theme's colors.
    return cms_get_theme_banner(request_theme(), epoch);
}
