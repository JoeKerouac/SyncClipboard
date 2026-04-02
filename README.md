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

## 通信协议

所有通信通过 WebSocket JSON 消息，流程如下：

1. **连接** → 客户端连接 WebSocket
2. **认证** → 发送 `{"type":"auth", "serverKey":"..."}` 验证服务器密钥
3. **登录** → 发送 `{"type":"login", "username":"...", "password":"...", "deviceId":"..."}` 登录
4. **同步** → 发送/接收 `{"type":"clipboard", "content":"<AES加密后的Base64内容>"}` 同步剪切板

内容使用 AES-256-CBC 加密，服务端不解密，仅转发。

## 安全机制

- **服务器密钥认证**：客户端需提供正确的 server_key 才能连接
- **用户名密码登录**：登录后才能收发剪切板
- **AES-256-CBC 加密**：剪切板内容在客户端加密/解密，服务端无法解密
- **用户隔离**：仅同步到相同用户名的其他在线设备

## 构建
执行 `build_all.sh` 即可构建；

# 配置
## Linux/Windows客户端配置

- server_host: 服务端域名/IP，默认: 127.0.0.1
- server_port: 服务端端口，默认: 8080
- server_path: 服务端api前缀，默认: /ws/clipboard
- server_key: 服务端密钥，用于服务端对客户端鉴权，错误将禁止连接，默认: my-secret-server-key
- username: 用户名，默认: admin
- password: 密码，用于用户登陆，登陆后才能进行同步，剪切板内容将会在相同的用户的不同设备间同步，默认: admin123
- aes_key: aes key，同一个用户的不同客户端需要配置为相同的值，同步的内容将会使用本密钥使用AES-256-CBC算法加密，默认: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
- device_id: 设备ID，用来标识同一个用户的不同客户端，例如: linux-device-01
- file_transfer_level: 文件传输级别，只影响图片和文件，默认的文本全部走服务器，如果服务器和多个客户端的值都不同，则取最小的，默认: 3
    - 0: 表示不传输文件；
    - 1: 表示只尝试使用LAN来传输，及两个客户端在同一个局域网时才尝试使用局域网传输；
    - 2: 在LAN不可用时，还会尝试通过服务器在不同的客户端进行NAT打洞，打洞成功后会使用该连接传输；
    - 3: 在LAN和NAT都不可用时，尝试使用服务器转发的形式传输文件，即先把文件传输到服务器，然后在转发给其他客户端；
- max_transfer_size: 文件传输最大大小，单位MB，默认: 500
- log_level: 日志级别，默认: 1
    - 0: DEBUG
    - 1: INFO
    - 2: WARN
    - 3: ERROR
- max_log_size_mb: 单个日志文件最大MB，超过自动滚动，只对Windows客户端生效，默认: 10



## 服务端配置
- server.port: 服务端端口，默认: 8080
- syncclipboard.server-key: 服务端密钥，用于服务端对客户端鉴权，错误将禁止连接，默认: my-secret-server-key
- syncclipboard.users-file: 存储用户名密码的properties文件，默认: user.properties
- syncclipboard.max-relay-size: 如果允许服务端转发文件，则这个可以设置服务端转发文件的最大大小，单位byte，默认: 307200
- syncclipboard.udp-port: 如果允许NAT打洞，则监听该端口用来辅助客户端NAT打洞，默认: 8081
- syncclipboard.file-transfer-level: 参考客户端配置

