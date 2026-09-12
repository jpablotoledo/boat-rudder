#include "geoip.h"
#include "../../utils/log.h"
#include <stdio.h>
#include <string.h>

#ifdef HAVE_MAXMINDDB
#include <maxminddb.h>

static MMDB_s mmdb;
static int mmdb_open = 0;

int geoip_init(const char *mmdb_path) {
    const char *path = (mmdb_path && mmdb_path[0]) ? mmdb_path : GEOIP_DEFAULT_DB_PATH;
    int status = MMDB_open(path, MMDB_MODE_MMAP, &mmdb);
    if (status != MMDB_SUCCESS) {
        LOG_WARN("[GeoIP] Failed to open database '%s': %s", path, MMDB_strerror(status));
        return -1;
    }
    mmdb_open = 1;
    LOG_INFO("[GeoIP] Database loaded: %s", path);
    return 0;
}

void geoip_cleanup(void) {
    if (mmdb_open) {
        MMDB_close(&mmdb);
        mmdb_open = 0;
    }
}

void geoip_lookup(const char *ip, char *out, size_t size) {
    if (!mmdb_open || !ip || ip[0] == '\0') {
        snprintf(out, size, "Unknown");
        return;
    }

    int gai_error = 0;
    int mmdb_error = 0;
    MMDB_lookup_result_s result = MMDB_lookup_string(&mmdb, ip, &gai_error, &mmdb_error);

    if (gai_error != 0 || mmdb_error != MMDB_SUCCESS || !result.found_entry) {
        snprintf(out, size, "Unknown");
        return;
    }

    MMDB_entry_data_s entry_data;

    // Try the full English name first ("Chile"), fall back to the ISO code
    // ("CL") for anything the database has no English name for.
    int status = MMDB_get_value(&result.entry, &entry_data, "country", "names", "en", NULL);
    if (status != MMDB_SUCCESS || !entry_data.has_data ||
        entry_data.type != MMDB_DATA_TYPE_UTF8_STRING || entry_data.data_size == 0) {
        status = MMDB_get_value(&result.entry, &entry_data, "country", "iso_code", NULL);
    }

    if (status == MMDB_SUCCESS && entry_data.has_data &&
        entry_data.type == MMDB_DATA_TYPE_UTF8_STRING &&
        entry_data.data_size > 0 && entry_data.data_size < size) {
        memcpy(out, entry_data.utf8_string, entry_data.data_size);
        out[entry_data.data_size] = '\0';
    } else {
        snprintf(out, size, "Unknown");
    }
}

#else // HAVE_MAXMINDDB not defined - GeoIP disabled at compile time

int geoip_init(const char *mmdb_path) {
    (void)mmdb_path;
    LOG_INFO("[GeoIP] libmaxminddb not available at build time - country detection disabled "
             "(every visit will show \"Unknown\"). Install libmaxminddb-dev and rebuild to enable it.");
    return 0;
}

void geoip_cleanup(void) {
    // No-op.
}

void geoip_lookup(const char *ip, char *out, size_t size) {
    (void)ip;
    snprintf(out, size, "Unknown");
}

#endif // HAVE_MAXMINDDB
