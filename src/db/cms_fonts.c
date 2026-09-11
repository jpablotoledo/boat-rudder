#include "cms_fonts.h"
#include "mongodb_manager.h"
#include "../utils/log.h"
#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FONTS_COLLECTION "fonts"
#define FONTS_LIST_LIMIT 200

void cms_get_fonts(CmsFont **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(FONTS_COLLECTION);
    if (!collection) return;

    bson_t *query = bson_new();
    bson_t *opts = BCON_NEW(
        "sort", "{", "name", BCON_INT32(1), "}",
        "limit", BCON_INT64((int64_t)FONTS_LIST_LIMIT)
    );

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

    CmsFont *fonts = calloc(FONTS_LIST_LIMIT, sizeof(CmsFont));
    if (!fonts) {
        bson_destroy(query);
        bson_destroy(opts);
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(collection);
        return;
    }

    size_t count = 0;
    const bson_t *doc;
    while (count < FONTS_LIST_LIMIT && mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;

        char id_str[25] = "";
        if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter))
            bson_oid_to_string(bson_iter_oid(&iter), id_str);

        fonts[count].id = strdup(id_str);
        fonts[count].name = (bson_iter_init_find(&iter, doc, "name") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");
        fonts[count].filename = (bson_iter_init_find(&iter, doc, "filename") && BSON_ITER_HOLDS_UTF8(&iter))
            ? strdup(bson_iter_utf8(&iter, NULL)) : strdup("");

        count++;
    }

    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        LOG_ERROR("cms_get_fonts: cursor error: %s", error.message);

    bson_destroy(query);
    bson_destroy(opts);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);

    *out = fonts;
    *out_count = count;
}

void cms_fonts_free(CmsFont *fonts, size_t count) {
    if (!fonts) return;
    for (size_t i = 0; i < count; i++) {
        free(fonts[i].id);
        free(fonts[i].name);
        free(fonts[i].filename);
    }
    free(fonts);
}

int cms_add_font(const char *name, const char *filename) {
    mongoc_collection_t *collection = mongodb_manager_get_collection(FONTS_COLLECTION);
    if (!collection) return -1;

    bson_t doc;
    bson_init(&doc);
    bson_append_utf8(&doc, "name", -1, name, -1);
    bson_append_utf8(&doc, "filename", -1, filename, -1);

    bson_error_t error;
    bool ok = mongoc_collection_insert_one(collection, &doc, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_add_font: insert failed: %s", error.message);

    bson_destroy(&doc);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

int cms_get_font_filename(const char *id_hex, char *out, size_t out_size) {
    out[0] = '\0';
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return 0;

    mongoc_collection_t *collection = mongodb_manager_get_collection(FONTS_COLLECTION);
    if (!collection) return 0;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    int found = 0;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "filename") && BSON_ITER_HOLDS_UTF8(&iter)) {
            strncpy(out, bson_iter_utf8(&iter, NULL), out_size - 1);
            out[out_size - 1] = '\0';
            found = 1;
        }
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return found;
}

int cms_delete_font(const char *id_hex) {
    if (!bson_oid_is_valid(id_hex, strlen(id_hex))) return -1;

    mongoc_collection_t *collection = mongodb_manager_get_collection(FONTS_COLLECTION);
    if (!collection) return -1;

    bson_oid_t oid;
    bson_oid_init_from_string(&oid, id_hex);
    bson_t *query = BCON_NEW("_id", BCON_OID(&oid));

    bson_error_t error;
    bool ok = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);
    if (!ok) LOG_ERROR("cms_delete_font: delete failed: %s", error.message);

    bson_destroy(query);
    mongoc_collection_destroy(collection);
    return ok ? 0 : -1;
}

char *cms_get_font_filename_by_name(const char *name) {
    if (!name || !name[0]) return strdup("");

    mongoc_collection_t *collection = mongodb_manager_get_collection(FONTS_COLLECTION);
    if (!collection) return strdup("");

    bson_t *query = BCON_NEW("name", BCON_UTF8(name));
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    char *result = NULL;
    const bson_t *doc;
    if (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "filename") && BSON_ITER_HOLDS_UTF8(&iter))
            result = strdup(bson_iter_utf8(&iter, NULL));
    }

    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    return result ? result : strdup("");
}

const char *cms_font_format_for_filename(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return "";
    if (strcasecmp(dot, ".woff2") == 0) return "woff2";
    if (strcasecmp(dot, ".woff") == 0) return "woff";
    if (strcasecmp(dot, ".ttf") == 0) return "truetype";
    if (strcasecmp(dot, ".otf") == 0) return "opentype";
    return "";
}
