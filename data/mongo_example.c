#include <mongoc/mongoc.h>
#include <stdio.h>

int main() {
    mongoc_init();

    // Crear cliente y conectar a MongoDB local
    mongoc_client_t *client = mongoc_client_new("mongodb://localhost:27017");
    if (!client) {
        fprintf(stderr, "Error: No se pudo conectar a MongoDB.\n");
        return 1;
    }

    // Acceder a la colección 'routes' en la base 'the_retro_center'
    mongoc_collection_t *collection = mongoc_client_get_collection(client, "the_retro_center", "routes");

    // Consulta vacía = traer todos los documentos
    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);

    const bson_t *doc;
    char *str;

    // Iterar sobre los documentos y mostrarlos en formato JSON
    while (mongoc_cursor_next(cursor, &doc)) {
        str = bson_as_canonical_extended_json(doc, NULL);
        printf("%s\n", str);
        bson_free(str);
    }

    // Limpieza
    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
    mongoc_cleanup();

    return 0;
}
