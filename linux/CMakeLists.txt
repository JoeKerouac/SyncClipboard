cmake_minimum_required(VERSION 3.10)
project(SyncClipboardLinux C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(XFIXES REQUIRED xfixes)

add_executable(sync_clipboard
    main.c
    ../common/config.c
    ../common/crypto.c
    ../common/cJSON.c
    ../common/file_transfer.c
    ../common/stb_impl.c
)

target_include_directories(sync_clipboard PRIVATE
    ../common
    ${LWS_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${XFIXES_INCLUDE_DIRS}
)

target_link_directories(sync_clipboard PRIVATE
    ${LWS_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
    ${X11_LIBRARY_DIRS}
    ${XFIXES_LIBRARY_DIRS}
)

target_link_libraries(sync_clipboard
    ${LWS_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${X11_LIBRARIES}
    ${XFIXES_LIBRARIES}
    pthread
    m
)
