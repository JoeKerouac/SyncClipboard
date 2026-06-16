cmake_minimum_required(VERSION 3.10)
project(SyncClipboardWindows C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(OPENSSL REQUIRED openssl)

add_executable(sync_clipboard
    main.c
    ../common/config.c
    ../common/crypto.c
    ../common/cJSON.c
    ../common/file_transfer.c
)

target_include_directories(sync_clipboard PRIVATE
    ../common
    ${LWS_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
)

target_link_directories(sync_clipboard PRIVATE
    ${LWS_LIBRARY_DIRS}
    ${OPENSSL_LIBRARY_DIRS}
)

target_link_libraries(sync_clipboard
    ${LWS_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ws2_32
    iphlpapi
)

# Build as GUI subsystem so no console window appears on auto-start
target_link_options(sync_clipboard PRIVATE -mwindows)
