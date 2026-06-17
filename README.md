# SyncClipboard - 剪切板多端共享

跨平台剪切板同步工具，支持 Windows、Linux、Android 三端通过 WebSocket 长连接实时同步剪切板内容。

## 架构

```
┌──────────┐     ┌──────────┐     ┌──────────┐
│  Windows │     │  Linux   │     │ Android  │
│  Client  │     │  Client  │     │  Client  │
│   (C)    │     │   (C)    │     │ (Kotlin) │
└────┬─────┘     └────┬─────┘     └────┬─────┘
     │                │                │
     │    WebSocket   │    WebSocket   │
     └────────┬───────┘────────┬───────┘
              │                │
         ┌────┴────────────────┴────┐
         │     Server (Java)        │
         │   Spring Boot WebSocket  │
         └──────────────────────────┘
```

## 通信协议 (v2)

1. **HTTP 登录** → 客户端 `POST /api/v2/auth/login` 获取 JWT
2. **WebSocket 握手** → 使用 `Sec-WebSocket-Protocol: bearer.<jwt>` 连接 `/ws/v2/clipboard`
3. **能力协商** → 服务端返回 `hello_ack` 告知文件传输级别等配置
4. **剪贴板同步** → 发送/接收 `{"type":"clipboard", "content":"<AES-GCM加密后的Base64内容>"}`
5. **文件传输** → 通过 `file_offer/file_request/file_peer_info` 系列消息协调 P2P 传输

内容使用 AES-256-GCM 端到端加密，服务端不解密，仅路由转发。

## 安全机制

- **JWT 认证**：客户端通过 HTTP 登录获取 access/refresh token，WebSocket 握手阶段验证
- **登录限流**：同一 (用户名, IP) 每分钟最多 5 次失败尝试
- **AES-256-GCM 加密**：剪切板内容在客户端加密/解密（带认证标签，防篡改），服务端无法解密
- **用户隔离**：仅同步到相同用户名的其他在线设备
- **密码 BCrypt 哈希**：用户密码以 BCrypt(12) 存储，不保存明文
- **TLS 可选**：生产环境推荐启用 HTTPS/WSS，通过配置开关控制

## 构建

```sh
# 一键构建全部（Server + Linux + Windows + Android）
./build_all.sh

# 首次 Windows 交叉编译前需先构建依赖
./build_windows_deps.sh
```

## 快速开始

```sh
# 1. 创建用户
cd server
java -jar target/sync-clipboard-server-1.0.0.jar user add <username> <password>

# 2. 启动服务端
java -jar target/sync-clipboard-server-1.0.0.jar

# 3. 配置客户端 config.ini 填入 server_host/username/password/aes_key/device_id

# 4. 启动客户端
./linux/build/sync_clipboard linux/config.ini
```

## 用户管理

通过 jar 内置命令行工具管理用户（不启动服务器）：

```sh
# 添加用户（密码自动 BCrypt 哈希）
java -jar sync-clipboard-server-1.0.0.jar user add <username> <password>

# 修改密码
java -jar sync-clipboard-server-1.0.0.jar user passwd <username> <newpassword>

# 列出所有用户
java -jar sync-clipboard-server-1.0.0.jar user list

# 删除用户
java -jar sync-clipboard-server-1.0.0.jar user remove <username>
```

用户文件存放在 `server/data/users.properties`，自动设为 600 权限。

---

# 配置

## 服务端配置

配置文件：`server/src/main/resources/application.properties`

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `server.port` | HTTP 监听端口 | `8080` |
| `syncclipboard.data-dir` | 数据目录（users/jwt.key） | `data` |
| `syncclipboard.users-file` | 用户文件路径 | `data/users.properties` |
| `syncclipboard.server-key` | 旧版 v1 认证密钥（v2 不使用，可留空） | 空（环境变量 `SYNC_SERVER_KEY`） |
| `syncclipboard.max-relay-size` | 服务端中转文件最大字节数 | `307200` (300KB) |
| `syncclipboard.udp-port` | NAT 打洞 UDP 反射器端口 | `8081` |
| `syncclipboard.file-transfer-level` | 文件传输级别（参考客户端说明） | `2` |
| `syncclipboard.login.max-attempts` | 每 (用户, IP) 登录失败上限 | `5` |
| `syncclipboard.login.window-seconds` | 限流窗口秒数 | `60` |

### TLS 配置（可选）

```properties
server.port=8443
server.ssl.enabled=true
server.ssl.key-store=data/server.p12
server.ssl.key-store-password=changeit
server.ssl.key-alias=syncclipboard
```

快速生成自签证书：`bash server/scripts/gen-self-signed.sh`

---

## Linux/Windows 客户端配置

配置文件：`config.ini`（与可执行文件同目录，或作为命令行参数传入）

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `server_host` | 服务端域名/IP | （必填） |
| `server_port` | 服务端端口 | `8080` |
| `server_path` | WebSocket 路径 | `/ws/v2/clipboard` |
| `use_tls` | 是否使用 TLS：0=http/ws, 1=https/wss | `0` |
| `server_key` | v1 遗留字段，v2 不使用 | （必填但不生效） |
| `username` | 用户名 | （必填） |
| `password` | 密码 | （必填） |
| `aes_key` | AES-256 加密密钥（64 位十六进制字符） | （必填） |
| `device_id` | 设备唯一标识 | （必填） |
| `file_transfer_level` | 文件传输级别 | `3` |
| `max_transfer_size` | P2P 文件传输最大大小（MB） | `500` |
| `log_level` | 日志级别：0=DEBUG, 1=INFO, 2=WARN, 3=ERROR | `1` |
| `max_log_size_mb` | 单个日志文件最大 MB（仅 Windows） | `10` |

### file_transfer_level 说明

| 值 | 说明 |
|----|------|
| 0 | 不传输文件（仅同步文本） |
| 1 | 仅 LAN 直连（同局域网时 P2P 传输） |
| 2 | LAN + NAT 打洞（尝试穿透 NAT） |
| 3 | LAN + NAT + 服务器中转（兜底通过服务器转发） |

> 实际级别取服务端与客户端配置的最小值。

### 配置示例

```ini
server_host = 192.168.1.100
server_port = 8080
server_path = /ws/v2/clipboard
use_tls = 0
server_key = unused
username = alice
password = mypassword
aes_key = a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4
device_id = my-linux-pc
file_transfer_level = 3
max_transfer_size = 500
```

---

## Android 客户端配置

在 App 设置界面填写以下字段：

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| Server Host | 服务端域名/IP | （必填） |
| Server Port | 服务端端口 | （必填） |
| Use TLS | 是否使用 TLS (https/wss) | 关闭 |
| Username | 用户名 | （必填） |
| Password | 密码 | （必填） |
| AES Key | 64 位十六进制加密密钥 | （必填） |
| Device ID | 设备唯一标识 | （必填） |

> 所有配置通过 EncryptedSharedPreferences 加密存储，`adb backup` 无法导出。

---

## 注意事项

- **AES Key 必须全端一致**：同一用户的所有设备必须配置相同的 64 位 hex AES 密钥，否则无法解密
- **TLS 推荐但非必须**：内网部署可关闭 TLS（`use_tls=0`），公网部署强烈建议开启
- **密码安全**：请勿使用默认示例密码，通过 `user add` 命令创建强密码用户
- **首次启动**：服务端在没有 `data/users.properties` 时会拒绝启动，必须先创建用户

## 文档

- [协议 v2 规范](docs/PROTOCOL_V2.md)
- [安全模型](docs/SECURITY.md)
- [v1 → v2 迁移指南](docs/MIGRATION.md)
