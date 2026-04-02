#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

JAVA_HOME="/home/joe/App/java/jdk17"
ANDROID_HOME="${ANDROID_HOME:-/home/joe/Android/Sdk}"
MVN_BIN="/home/joe/App/maven/apache-maven-3.9.5/bin/mvn"
MVN_SETTINGS="/home/joe/App/maven/settings.xml"
WIN64_PREFIX="$SCRIPT_DIR/windows/deps/win64"

export JAVA_HOME ANDROID_HOME

RESULTS=()
record() { RESULTS+=("$1"); }

# ============================================================
# Server (Java / Maven)
# ============================================================
build_server() {
    echo -e "${YELLOW}[1/4] 编译 Server (Java/Maven)...${NC}"
    cd "$SCRIPT_DIR/server"
    "$MVN_BIN" --settings "$MVN_SETTINGS" clean package -DskipTests -q
    echo -e "${GREEN}  ✔ server/target/sync-clipboard-server-1.0.0.jar${NC}"
    record "Server:    OK"
}

# ============================================================
# Linux Client (C / CMake)
# ============================================================
build_linux() {
    echo -e "${YELLOW}[2/4] 编译 Linux Client (C/CMake)...${NC}"
    cd "$SCRIPT_DIR/linux"
    rm -rf build && mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    make -j"$(nproc)" > /dev/null 2>&1
    echo -e "${GREEN}  ✔ linux/build/sync_clipboard${NC}"
    record "Linux:     OK"
}

# ============================================================
# Windows Client (C / MinGW cross-compile)
# ============================================================
build_windows() {
    echo -e "${YELLOW}[3/4] 编译 Windows Client (MinGW 交叉编译)...${NC}"

    if ! command -v x86_64-w64-mingw32-gcc &>/dev/null; then
        echo -e "${RED}  ✘ 未安装 mingw-w64，跳过 (sudo apt install gcc-mingw-w64-x86-64)${NC}"
        record "Windows:   SKIP (no mingw)"
        return
    fi

    if [ ! -f "$WIN64_PREFIX/lib/libwebsockets_static.a" ]; then
        echo -e "${RED}  ✘ Windows 依赖库未编译，跳过 (先运行 build_windows_deps.sh)${NC}"
        record "Windows:   SKIP (no deps)"
        return
    fi

    cd "$SCRIPT_DIR/windows"
    mkdir -p build
    x86_64-w64-mingw32-gcc -std=c11 \
        -I"$WIN64_PREFIX/include" \
        -I"$SCRIPT_DIR/common" \
        -o build/sync_clipboard.exe \
        main.c \
        ../common/config.c \
        ../common/crypto.c \
        ../common/cJSON.c \
        ../common/file_transfer.c \
        ../common/stb_impl.c \
        -L"$WIN64_PREFIX/lib" \
        -L"$WIN64_PREFIX/lib64" \
        -lwebsockets_static -lssl -lcrypto \
        -lws2_32 -lgdi32 -lcrypt32 -liphlpapi -lpthread -lshlwapi -lshell32 \
        -static 2>/dev/null

    echo -e "${GREEN}  ✔ windows/build/sync_clipboard.exe${NC}"
    record "Windows:   OK"
}

# ============================================================
# Android Client (Kotlin / Gradle)
# ============================================================
build_android() {
    echo -e "${YELLOW}[4/4] 编译 Android Client (Gradle)...${NC}"
    cd "$SCRIPT_DIR/android"

    if [ ! -f gradlew ]; then
        echo -e "${RED}  ✘ 缺少 gradlew，跳过${NC}"
        record "Android:   SKIP (no gradlew)"
        return
    fi

    chmod +x gradlew
    ./gradlew assembleDebug -q
    echo -e "${GREEN}  ✔ android/app/build/outputs/apk/debug/app-debug.apk${NC}"
    record "Android:   OK"
}

# ============================================================
# 主流程
# ============================================================
echo "========================================="
echo " SyncClipboard 全量编译"
echo "========================================="
echo ""

build_server
build_linux
build_windows
build_android

echo ""
echo "========================================="
echo " 编译结果"
echo "========================================="
for r in "${RESULTS[@]}"; do
    echo "  $r"
done
echo ""
