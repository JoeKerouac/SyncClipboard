#!/bin/bash
#
# Windows 交叉编译依赖构建（OpenSSL + libwebsockets）
# 仅首次需要运行，编译产物在 windows/deps/win64/
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="$SCRIPT_DIR/windows/deps"
PREFIX="$DEPS_DIR/win64"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

if ! command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    echo -e "${RED}请先安装 mingw-w64: sudo apt install gcc-mingw-w64-x86-64${NC}"
    exit 1
fi

mkdir -p "$DEPS_DIR"
cd "$DEPS_DIR"

# ---- OpenSSL ----
OPENSSL_VER="3.0.13"
if [ ! -f "$PREFIX/lib64/libssl.a" ]; then
    echo -e "${YELLOW}[1/2] 编译 OpenSSL ${OPENSSL_VER} for Windows...${NC}"
    if [ ! -d "openssl-${OPENSSL_VER}" ]; then
        curl -sL "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz" -o openssl.tar.gz
        tar xzf openssl.tar.gz
    fi
    cd "openssl-${OPENSSL_VER}"
    ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- --prefix="$PREFIX" no-shared > /dev/null 2>&1
    make -j"$(nproc)" > /dev/null 2>&1
    make install_sw > /dev/null 2>&1
    cd "$DEPS_DIR"
    echo -e "${GREEN}  ✔ OpenSSL 编译完成${NC}"
else
    echo -e "${GREEN}[1/2] OpenSSL 已存在，跳过${NC}"
fi

# ---- libwebsockets ----
LWS_VER="4.3.3"
if [ ! -f "$PREFIX/lib/libwebsockets_static.a" ]; then
    echo -e "${YELLOW}[2/2] 编译 libwebsockets ${LWS_VER} for Windows...${NC}"
    if [ ! -d "libwebsockets-${LWS_VER}" ]; then
        curl -sL "https://github.com/warmcat/libwebsockets/archive/refs/tags/v${LWS_VER}.tar.gz" -o lws.tar.gz
        tar xzf lws.tar.gz
    fi
    mkdir -p lws-build && cd lws-build
    cmake "../libwebsockets-${LWS_VER}" \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DOPENSSL_ROOT_DIR="$PREFIX" \
        -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
        -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib64/libcrypto.a" \
        -DOPENSSL_SSL_LIBRARY="$PREFIX/lib64/libssl.a" \
        -DLWS_WITH_SSL=ON \
        -DLWS_WITHOUT_TESTAPPS=ON \
        -DLWS_WITHOUT_TEST_SERVER=ON \
        -DLWS_WITHOUT_TEST_CLIENT=ON \
        -DLWS_WITH_SHARED=OFF \
        -DLWS_WITH_STATIC=ON \
        -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
        -DLWS_IPV6=OFF \
        > /dev/null 2>&1
    make -j"$(nproc)" > /dev/null 2>&1
    make install > /dev/null 2>&1
    cd "$DEPS_DIR"
    echo -e "${GREEN}  ✔ libwebsockets 编译完成${NC}"
else
    echo -e "${GREEN}[2/2] libwebsockets 已存在，跳过${NC}"
fi

echo ""
echo -e "${GREEN}Windows 依赖编译完成，产物目录: $PREFIX${NC}"
