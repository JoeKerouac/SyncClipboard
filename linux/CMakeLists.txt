cmake_minimum_required(VERSION 3.10)
project(SyncClipboardLinux C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(XFIXES REQUIRED xfixes)
pkg_check_modules(CURL REQUIRED libcurl)

add_executable(sync_clipboard
    main.c
    ../common/config.c
    ../common/crypto.c
    ../common/cJSON.c
    ../common/ft/ft_socket.c
    ../common/ft/ft_addr.c
    ../common/ft/ft_lan.c
    ../common/ft/ft_nat.c
    ../common/ft/ft_proto.c
    ../common/ft/ft_b64_sha.c
    ../common/ft/ft_auth.c
    ../common/stb_impl.c
    ../common/log.c
    ../common/codec.c
    ../common/auth_http.c
    ../common/ws_client.c
    ../common/msg.c
)

target_include_directories(sync_clipboard PRIVATE
    ../common
    ${LWS_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${XFIXES_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_directories(sync_clipboard PRIVATE
    ${LWS_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
    ${X11_LIBRARY_DIRS}
    ${XFIXES_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
)

target_link_libraries(sync_clipboard
    ${LWS_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${X11_LIBRARIES}
    ${XFIXES_LIBRARIES}
    ${CURL_LIBRARIES}
    pthread
    m
)
