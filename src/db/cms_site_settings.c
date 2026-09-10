#include "cms_site_settings.h"
#include "mongodb_manager.h"
#include "../utils/config_loader.h"
#include "../utils/theme_catalog.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

#define SITE_SETTINGS_COLLECTION "site_settings"
#define SITE_NAME_DEFAULT "Boat Rudder"

// site_settings is a singleton: every write is an upsert against the empty
// filter {}, same shape as mongoc_collection_find_with_opts()'s read side
// below - the first save creates the document, every later save updates it.
static int upsert_set(bson_t *set_doc_owner /* consumed */) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) {
        bson_destroy(set_doc_owner);
        return -1;
    }

    bson_t *query = bson_new();
    bson_t *update = bson_new();
    BSON_APPEND_DOCUMENT(update, "$set", set_doc_owner);
    bson_t *opts = BCON_NEW("upsert", BCON_BOOL(true));

    bson_error_t error;
    bool ok = mongoc_collection_update_one(collection, query, update, opts, NULL, &error);
    if (!ok) LOG_ERROR("cms_site_settings: upsert failed: %s", error.message);

    bson_destroy(set_doc_owner);
    bson_destroy(query);
    bson_destroy(update);
    bson_destroy(opts);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

char *cms_get_site_name(void) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (!collection) return strdup(SITE_NAME_DEFAULT);

    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "site_name") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup(SITE_NAME_DEFAULT);
}

int cms_update_site_name(const char *name) {
    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "site_name", -1, name, -1);
    return upsert_set(set_doc);
}

char *cms_get_active_theme_key(void) {
    char *db_value = NULL;

    mongoc_collection_t *collection = mongodb_manager_get_collection(SITE_SETTINGS_COLLECTION);
    if (collection) {
        bson_t *query = bson_new();
        mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

        const bson_t *doc;
        if (mongoc_cursor_next(cursor, &doc)) {
            bson_iter_t iter;
            if (bson_iter_init_find(&iter, doc, "active_theme") && BSON_ITER_HOLDS_UTF8(&iter))
                db_value = strdup(bson_iter_utf8(&iter, NULL));
        }

        bson_destroy(query);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
    }

    if (db_value && theme_key_is_valid(db_value)) return db_value;
    free(db_value);
    return strdup(theme); // configs/settings.conf fallback - see request_theme.h
}

int cms_set_active_theme(const char *key) {
    if (!theme_key_is_valid(key)) return -1;

    bson_t *set_doc = bson_new();
    bson_append_utf8(set_doc, "active_theme", -1, key, -1);
    return upsert_set(set_doc);
}
