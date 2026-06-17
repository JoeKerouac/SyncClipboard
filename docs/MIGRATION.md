# Migration Guide: v1 → v2

## Breaking Changes

Protocol v2 is **not backward compatible** with v1. All clients and the
server must be upgraded together.

### What Changed

| Area | v1 | v2 |
|------|----|----|
| Transport | ws:// (plaintext) | wss:// (TLS mandatory) |
| Auth | WebSocket `auth` + `login` messages (plaintext password) | HTTP JWT login + WS `Sec-WebSocket-Protocol` handshake |
| Encryption | AES-256-CBC (no auth tag) | AES-256-GCM (AEAD) |
| WS path | `/ws/clipboard` | `/ws/v2/clipboard` |
| Default credentials | Built-in admin/admin123 | None — must be created via CLI |
| Android config | Plain SharedPreferences + allowBackup=true | EncryptedSharedPreferences + backup disabled |

## Upgrade Steps

### 1. Server

```sh
# Create users (admin/admin123 no longer exists)
java -cp sync-clipboard-server-2.0.0.jar \
  com.syncclipboard.cli.UserAdminCommand add <username> <password>

# Generate TLS certificate (optional for LAN-only; required for production)
cd server && bash scripts/gen-self-signed.sh

# Set environment variables
export SYNC_SERVER_KEY=<random-string>  # can be empty for v2 (JWT replaces it)

# Start
java -jar sync-clipboard-server-2.0.0.jar
```

### 2. Android

After installing the v2 APK:
1. Open the app; it will detect empty configuration and show the settings screen.
2. Enter the server host/port (use port 8443 if TLS is enabled), username,
   password, AES key (64 hex chars), and device ID.
3. If using a self-signed certificate, install the CA on the device
   (Settings → Security → Install from storage).

The app automatically migrates old preferences to EncryptedSharedPreferences.

### 3. Linux / Windows

Update `config.ini`:
- Ensure all required fields are present (empty defaults are now rejected).
- The client will HTTP-login to obtain a JWT before connecting the WebSocket.
- Ensure `libcurl` is installed (`apt install libcurl4-openssl-dev` on Linux).

### 4. Security Hygiene (Mandatory)

- **Change all passwords** — the old `admin123` has been in git history.
- **Rotate AES keys** if the 64-hex key was ever committed or shared insecurely.
- **Rotate server-key** or simply rely on JWT (server-key is deprecated in v2).

## Rollback

If v2 clients encounter a v1 server, the server responds with close code 1003
("protocol upgrade required"). There is no graceful fallback — downgrade all
components back to v1.0 if you need to rollback.
