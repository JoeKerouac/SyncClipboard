# Protocol V2 Specification

## Overview

SyncClipboard Protocol V2 replaces the original plaintext WebSocket protocol
with an authenticated, encrypted pipeline.

### Transport

- **Mandatory TLS**: all connections use `wss://` (or `https://` for REST).
- **Endpoint**: `/ws/v2/clipboard`
- **Subprotocol**: `Sec-WebSocket-Protocol: bearer.<access_jwt>`

### Authentication Flow

1. Client calls `POST /api/v2/auth/login` with `{username, password, deviceId}`.
2. Server responds `{accessToken, refreshToken, expiresInSec}`.
3. Client connects WebSocket with `Sec-WebSocket-Protocol: bearer.<accessToken>`.
4. Server validates JWT during handshake and sends `hello_ack` on success.
5. Tokens refresh via `POST /api/v2/auth/refresh` when accessToken expires.

### Messages (JSON text frames)

All messages include `"type"` and optionally `"v":2`, `"msgId"`, `"ts"`.

| type | direction | description |
|------|-----------|-------------|
| `hello` | C→S | client announces capabilities |
| `hello_ack` | S→C | server confirms capabilities & config |
| `clipboard` | C→S / S→C | encrypted clipboard content |
| `file_offer` | C→S | sender registers a file for transfer |
| `file_notify` | S→C | server notifies receivers of available file |
| `file_request` | C→S | receiver requests peer info |
| `file_peer_info` | S→C | server sends peer connection details |
| `file_relay` | C→S | client pushes/requests relay data |
| `file_relay_request` | S→C | server asks sender for relay data |
| `file_relay_data` | S→C | server delivers relayed file data |
| `file_transfer_result` | C→S | client reports transfer outcome |
| `error` | S→C | error with `code`, `message`, `retryable` |
| `ping` / `pong` | both | keepalive |

### Error Codes

`AUTH_REQUIRED`, `AUTH_INVALID`, `RATE_LIMITED`, `PROTO_UNSUPPORTED`,
`MSG_INVALID`, `FILE_OFFER_NOT_FOUND`, `FILE_TOO_LARGE`,
`FILE_TRANSFER_DISABLED`, `INTERNAL_ERROR`

### Content Encryption

Clipboard content is encrypted client-side before transmission.

- **Algorithm**: AES-256-GCM
- **Frame**: `0x02 || nonce(12) || ciphertext || tag(16)`, Base64URL encoded
- **Key**: Pre-shared 256-bit hex key (64 hex chars)

The server never decrypts clipboard content — it simply routes the opaque blob
to other devices owned by the same user.
