#define _XOPEN_SOURCE 700

#include "db/cms_languages.h"
#include "db/mongodb_manager.h"
#include "modules/analytics/geoip.h"
#include "utils/config_loader.h"
#include "utils/log.h"
#include "web_server/server_listener.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// _Atomic provides formal memory-ordering guarantees for signal handler /
// main thread coordination. volatile alone does not guarantee atomicity.
static _Atomic int running = 1;

static void handle_shutdown(int signum) {
    (void)signum;
    atomic_store(&running, 0);
}

static void sigchld_handler(int s) {
    (void)s;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-c <config>] <root_directory>\n"
            "\n"
            "  -c <config>      Path to config file (default: ./configs/settings.conf)\n"
            "  <root_directory> Directory to serve\n",
            prog);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    const char *config_path  = "./configs/settings.conf";
    const char *root_arg     = NULL;

    // Parse arguments: optional -c <path> before the mandatory root_directory.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -c requires a path argument\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            config_path = argv[++i];
        } else if (!root_arg) {
            root_arg = argv[i];
        } else {
            fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!root_arg) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char resolved[PATH_MAX];
    if (!realpath(root_arg, resolved)) {
        fprintf(stderr, "Invalid root directory '%s': %s\n", root_arg, strerror(errno));
        return EXIT_FAILURE;
    }

    char *root_directory = strdup(resolved);
    if (!root_directory) {
        fprintf(stderr, "strdup: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (load_config(config_path) != 0) {
        fprintf(stderr, "Error loading config: %s\n", config_path);
        free(root_directory);
        return EXIT_FAILURE;
    }

    log_level = verbose_level;
    LOG_INFO("Boat Rudder starting");
    LOG_INFO("Config         : %s", config_path);
    LOG_INFO("Root directory : %s", root_directory);
    LOG_INFO("HTTP port      : %d", http_port);
    if (ssl_enabled) {
        LOG_INFO("HTTPS port     : %d", https_port);
        LOG_INFO("TLS cert       : %s", ssl_cert);
        LOG_INFO("TLS key        : %s", ssl_key);
    }
    if (trusted_proxies[0])
        LOG_INFO("Trusted proxies: %s", trusted_proxies);

    if (mongodb_manager_init(mongodb_uri, mongodb_db) != 0) {
        LOG_WARN("MongoDB unavailable - /login and /dashboard will return 503");
    } else {
        cms_languages_ensure_seeded();
    }

    geoip_init(GEOIP_DEFAULT_DB_PATH);

    signal(SIGINT,  handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGCHLD, sigchld_handler);

    if (server_start(root_directory, ssl_enabled, ssl_cert, ssl_key,
                     http_port, https_port) != 0) {
        LOG_ERROR("Failed to start server");
        free(root_directory);
        return EXIT_FAILURE;
    }

    while (atomic_load(&running)) sleep(1);

    LOG_INFO("Shutting down...");
    server_stop();
    geoip_cleanup();
    mongodb_manager_cleanup();
    free(root_directory);

    return EXIT_SUCCESS;
}
