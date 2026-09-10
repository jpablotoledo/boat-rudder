cmake_minimum_required(VERSION 3.10)
project(boat-rudder C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_definitions(-D_XOPEN_SOURCE=700)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug build: enabling AddressSanitizer")
    add_compile_options(-fsanitize=address -g -O0)
    add_link_options(-fsanitize=address)
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# ── Sources ───────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.c

    # Web server core
    src/web_server/server_listener.c
    src/web_server/connection.c
    src/web_server/connection_thread.c
    src/web_server/tls_context.c
    src/web_server/http_router.c
    src/web_server/http_request_parser.c

    # Web server utilities
    src/web_server/utils/url_parser.c
    src/web_server/utils/static_file_server.c

    # General utilities
    src/utils/config_loader.c
    src/utils/image_size.c
    src/utils/category_tags.c
    src/html_builder/page_layout.c
    src/utils/log.c
    src/utils/http_utils.c
    src/utils/detect_epoch.c
    src/utils/read_file.c
    src/utils/generate_url_theme.c
    src/utils/template_utils.c
    src/utils/build_epoch_response.c
    src/utils/json_utils.c
    src/utils/qr_generator/qr_generator.c
    src/utils/request_lang.c
    src/utils/request_theme.c
    src/utils/theme_catalog.c
    src/utils/request_user.c

    # Retro-compatible CMS: HTML builder
    src/html_builder/orchestrator.c
    src/modules/menu/menu.c
    src/modules/mainbanner/mainbanner.c
    src/modules/home_content/home_content.c
    src/modules/blog_list/blog_list.c
    src/modules/category_menu/category_menu.c
    src/modules/entry_page/entry_page.c
    src/modules/language_page/language_page.c
    src/modules/theme_page/theme_page.c
    src/modules/login/login.c
    src/modules/dashboard/dashboard.c
    src/modules/error/error.c
    src/modules/categories_admin/categories_admin.c
    src/modules/languages_admin/languages_admin.c
    src/modules/entries_admin/entries_admin.c
    src/modules/menu_admin/menu_admin.c
    src/modules/users_admin/users_admin.c
    src/modules/entry_editor/entry_editor.c
    src/modules/entry_editor/entry_editor_blocks.c
    src/modules/media_admin/media_admin.c
    src/modules/site_settings_admin/site_settings_admin.c

    # Web server utilities (multipart)
    src/web_server/utils/multipart_parser.c

    # Database / auth / sessions
    src/db/mongodb_manager.c
    src/db/auth.c
    src/db/bson_lang.c
    src/db/cms_entries.c
    src/db/cms_entries_admin.c
    src/db/cms_menu.c
    src/db/cms_categories.c
    src/db/cms_languages.c
    src/db/cms_users_admin.c
    src/db/cms_media.c
    src/db/cms_media_galleries.c
    src/db/cms_site_settings.c
    src/db/cms_themes.c
    src/db/language_catalog.c
    src/db/session_manager.c
)

add_executable(boat-rudder ${SOURCES})

target_include_directories(boat-rudder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
)

# libqrencode: retro epochs render YouTube blocks as a scannable QR code
# (no iframe before HTML5), so the encoder is a hard dependency. Ships no
# pkg-config file; qrencode_minimal.h declares the two symbols we use.
find_library(QRENCODE_LIBRARY NAMES qrencode libqrencode.so.4)
if(NOT QRENCODE_LIBRARY)
    message(FATAL_ERROR "libqrencode not found - install libqrencode4 (runtime) or libqrencode-dev")
endif()

target_link_libraries(boat-rudder
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    PkgConfig::MONGOC
    PkgConfig::SODIUM
    ${QRENCODE_LIBRARY}
)

install(TARGETS boat-rudder RUNTIME DESTINATION bin)
