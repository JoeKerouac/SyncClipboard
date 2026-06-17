#!/bin/bash
set -e

DIR="${SYNC_DATA_DIR:-data}"
mkdir -p "$DIR"

KEYSTORE="$DIR/server.p12"
ALIAS="syncclipboard"
PASS="changeit"

if [ -f "$KEYSTORE" ]; then
    echo "Keystore already exists: $KEYSTORE"
    exit 0
fi

keytool -genkeypair \
    -alias "$ALIAS" \
    -keyalg RSA \
    -keysize 2048 \
    -storetype PKCS12 \
    -keystore "$KEYSTORE" \
    -storepass "$PASS" \
    -validity 3650 \
    -dname "CN=SyncClipboard,O=SelfSigned"

chmod 600 "$KEYSTORE"

echo "=== Self-signed keystore created ==="
echo "  Path:     $KEYSTORE"
echo "  Alias:    $ALIAS"
echo "  Password: $PASS"
echo ""
echo "Add these lines to application.properties:"
echo "  server.ssl.enabled=true"
echo "  server.ssl.key-store=$KEYSTORE"
echo "  server.ssl.key-store-password=$PASS"
echo "  server.ssl.key-alias=$ALIAS"
echo "  server.port=8443"
