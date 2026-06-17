# Security Model

## Threat Landscape

SyncClipboard handles highly sensitive data (passwords, 2FA codes, private
text, screenshots). The v2 architecture addresses the following threats:

| Threat | Mitigation |
|--------|-----------|
| Network eavesdropping | Mandatory TLS (wss:// + HTTPS) |
| Credential stuffing | BCrypt password hashing + per-IP/user rate limiter |
| Replay attacks | Short-lived JWT access tokens (24h) + refresh rotation |
| Content tampering | AES-256-GCM authenticated encryption (AEAD) |
| Padding oracle | Replaced AES-CBC with AES-GCM |
| Data at rest (Android) | EncryptedSharedPreferences (AES-256-GCM master key) |
| ADB backup | `allowBackup="false"` + `dataExtractionRules` |
| Default credentials | Removed entirely; first-run requires CLI user creation |
| Sensitive log exposure | Payload logging removed; only type+length printed |
| Timing side-channel (HMAC) | `CRYPTO_memcmp` / `MessageDigest.isEqual` |

## Key Management

- **JWT signing key**: auto-generated 512-bit random secret at
  `server/data/jwt.key` (POSIX perms `rw-------`). Rotating the file
  invalidates all tokens.
- **AES content key**: shared out-of-band between all user's devices.
  Never transmitted over the wire; only the ciphertext travels.
- **User passwords**: stored as BCrypt(12) hashes in
  `server/data/users.properties`.

## Rate Limiting

Login attempts are limited to 5 per minute per (username, IP) combination.
After exceeding the limit, the server responds 429 / `RATE_LIMITED` and
closes the WebSocket if applicable.

## TLS Configuration

For self-hosted deployments, generate a self-signed certificate:

```sh
cd server && ./scripts/gen-self-signed.sh
```

This creates `data/server.p12` and prints the CA fingerprint for pinning.
