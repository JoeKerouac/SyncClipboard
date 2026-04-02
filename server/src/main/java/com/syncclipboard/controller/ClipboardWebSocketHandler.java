package com.syncclipboard.controller;

import com.google.gson.Gson;
import com.syncclipboard.model.ClientSession;
import com.syncclipboard.model.Message;
import com.syncclipboard.service.ClipboardCacheService;
import com.syncclipboard.service.FileTransferService;
import com.syncclipboard.service.SessionManager;
import com.syncclipboard.service.UserService;

import java.util.List;
import java.util.Map;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;
import org.springframework.web.socket.*;
import org.springframework.web.socket.handler.TextWebSocketHandler;

@Component
public class ClipboardWebSocketHandler extends TextWebSocketHandler {

    private static final Logger log = LoggerFactory.getLogger(ClipboardWebSocketHandler.class);

    private final SessionManager sessionManager;
    private final UserService userService;
    private final ClipboardCacheService clipboardCache;
    private final FileTransferService fileTransferService;
    private final Gson gson = new Gson();

    @Value("${syncclipboard.server-key:default-server-key}")
    private String serverKey;

    @Value("${syncclipboard.udp-port:8081}")
    private int udpPort;

    @Value("${syncclipboard.file-transfer-level:2}")
    private int fileTransferLevel;

    public ClipboardWebSocketHandler(SessionManager sessionManager, UserService userService,
                                     ClipboardCacheService clipboardCache,
                                     FileTransferService fileTransferService) {
        this.sessionManager = sessionManager;
        this.userService = userService;
        this.clipboardCache = clipboardCache;
        this.fileTransferService = fileTransferService;
    }

    @Override
    public void afterConnectionEstablished(WebSocketSession session) {
        sessionManager.addSession(session);
        log.info("[CONNECT] 新客户端连接 sessionId={}, remoteAddr={}", session.getId(), session.getRemoteAddress());
    }

    @Override
    protected void handleTextMessage(WebSocketSession session, TextMessage textMessage) throws Exception {
        String payload = textMessage.getPayload();
        log.debug("[RECV] sessionId={}, payload={}", session.getId(), payload);

        Message msg;
        try {
            msg = gson.fromJson(payload, Message.class);
        } catch (Exception e) {
            log.warn("[RECV] 无效JSON sessionId={}, payload={}", session.getId(), payload);
            sendResponse(session, "error", false, "Invalid JSON format");
            return;
        }

        if (msg.getType() == null) {
            log.warn("[RECV] 缺少type字段 sessionId={}", session.getId());
            sendResponse(session, "error", false, "Missing message type");
            return;
        }

        ClientSession clientSession = sessionManager.getSession(session.getId());
        if (clientSession == null) {
            log.warn("[RECV] 未找到会话 sessionId={}, 关闭连接", session.getId());
            session.close();
            return;
        }

        log.info("[RECV] sessionId={}, type={}", session.getId(), msg.getType());

        switch (msg.getType()) {
            case "auth" -> handleAuth(session, clientSession, msg);
            case "login" -> handleLogin(session, clientSession, msg);
            case "clipboard" -> handleClipboard(session, clientSession, msg);
            case "file_offer" -> handleFileOffer(session, clientSession, msg);
            case "file_request" -> handleFileRequest(session, clientSession, msg);
            case "file_relay" -> handleFileRelay(session, clientSession, msg);
            case "file_transfer_result" -> handleFileTransferResult(session, clientSession, msg);
            default -> {
                log.warn("[RECV] 未知消息类型 sessionId={}, type={}", session.getId(), msg.getType());
                sendResponse(session, "error", false, "Unknown message type: " + msg.getType());
            }
        }
    }

    private void handleAuth(WebSocketSession session, ClientSession clientSession, Message msg) throws Exception {
        if (serverKey.equals(msg.getServerKey())) {
            clientSession.setAuthenticated(true);
            sendResponse(session, "auth_result", true, "Authentication successful");
            log.info("[AUTH] 认证成功 sessionId={}", session.getId());
        } else {
            sendResponse(session, "auth_result", false, "Invalid server key");
            log.warn("[AUTH] 认证失败(密钥错误) sessionId={}", session.getId());
        }
    }

    private void handleLogin(WebSocketSession session, ClientSession clientSession, Message msg) throws Exception {
        if (!clientSession.isAuthenticated()) {
            log.warn("[LOGIN] 未认证就尝试登录 sessionId={}, user={}", session.getId(), msg.getUsername());
            sendResponse(session, "login_result", false, "Not authenticated, please authenticate first");
            return;
        }
        if (userService.authenticate(msg.getUsername(), msg.getPassword())) {
            clientSession.setLoggedIn(true);
            clientSession.setUsername(msg.getUsername());
            clientSession.setDeviceId(msg.getDeviceId());
            Map<String, Object> loginOk = new java.util.LinkedHashMap<>();
            loginOk.put("type", "login_result");
            loginOk.put("success", true);
            loginOk.put("message", "Login successful");
            loginOk.put("fileTransferLevel", fileTransferLevel);
            session.sendMessage(new TextMessage(gson.toJson(loginOk)));
            log.info("[LOGIN] 登录成功 sessionId={}, user={}, device={}",
                    session.getId(), msg.getUsername(), msg.getDeviceId());
            log.info("[STATUS] 当前在线会话数: {}", sessionManager.getOnlineCount());

            String cached = clipboardCache.getForDevice(msg.getUsername(), msg.getDeviceId());
            if (cached != null) {
                session.sendMessage(new TextMessage(cached));
                log.info("[LOGIN] 已下发缓存剪切板给 device={}", msg.getDeviceId());
            }
        } else {
            sendResponse(session, "login_result", false, "Invalid username or password");
            log.warn("[LOGIN] 登录失败(密码错误) sessionId={}, user={}", session.getId(), msg.getUsername());
        }
    }

    private void handleClipboard(WebSocketSession session, ClientSession clientSession, Message msg) throws Exception {
        if (!clientSession.isLoggedIn()) {
            log.warn("[CLIPBOARD] 未登录就发送剪切板 sessionId={}", session.getId());
            sendResponse(session, "error", false, "Not logged in");
            return;
        }

        long startMs = System.currentTimeMillis();
        int contentLen = msg.getContent() != null ? msg.getContent().length() : 0;
        log.info("[CLIPBOARD] 收到剪切板内容 user={}, device={}, contentLen={}",
                clientSession.getUsername(), clientSession.getDeviceId(), contentLen);

        clipboardCache.store(clientSession.getUsername(), msg.getContent(), clientSession.getDeviceId());

        String broadcastJson = gson.toJson(new ClipboardBroadcast(
                "clipboard", msg.getContent(), clientSession.getDeviceId()));
        var results = sessionManager.broadcastToUser(clientSession.getUsername(), session.getId(), broadcastJson);
        long totalElapsed = System.currentTimeMillis() - startMs;

        int sent = (int) results.stream().filter(SessionManager.SendResult::success).count();
        log.info("[CLIPBOARD] 同步完成 user={}, 目标{}个, 成功{}个, 总耗时{}ms",
                clientSession.getUsername(), results.size(), sent, totalElapsed);
        for (var r : results) {
            if (r.success()) {
                log.info("[CLIPBOARD]   -> device={}, 发送耗时{}ms", r.deviceId(), r.elapsedMs());
            } else {
                log.warn("[CLIPBOARD]   -> device={}, 发送失败: {}", r.deviceId(), r.error());
            }
        }
    }

    private void sendResponse(WebSocketSession session, String type, boolean success, String message) throws Exception {
        String json = gson.toJson(new Response(type, success, message));
        log.debug("[SEND] sessionId={}, response={}", session.getId(), json);
        session.sendMessage(new TextMessage(json));
    }

    @Override
    public void afterConnectionClosed(WebSocketSession session, CloseStatus status) {
        ClientSession cs = sessionManager.getSession(session.getId());
        String userInfo = cs != null && cs.getUsername() != null
                ? " user=" + cs.getUsername() + " device=" + cs.getDeviceId()
                : "";
        log.info("[DISCONNECT] 客户端断开 sessionId={},{} status={}", session.getId(), userInfo, status);
        sessionManager.removeSession(session.getId());
        log.info("[STATUS] 当前在线会话数: {}", sessionManager.getOnlineCount());
    }

    @Override
    public void handleTransportError(WebSocketSession session, Throwable exception) {
        log.error("[ERROR] 传输错误 sessionId={}, error={}", session.getId(), exception.getMessage());
        sessionManager.removeSession(session.getId());
    }

    // --- File transfer handlers ---

    private void handleFileOffer(WebSocketSession session, ClientSession cs, Message msg) throws Exception {
        if (!cs.isLoggedIn()) { sendResponse(session, "error", false, "Not logged in"); return; }
        if (fileTransferLevel <= 0) {
            sendResponse(session, "error", false, "File transfer is disabled on server");
            return;
        }
        String publicAddr = session.getRemoteAddress() != null
                ? session.getRemoteAddress().getAddress().getHostAddress() : "unknown";
        var offer = new FileTransferService.FileOffer(
                msg.getFileId(), msg.getFileName(), msg.getMimeType(), msg.getFileSize(),
                msg.getChecksum(), session.getId(), cs.getDeviceId(), cs.getUsername(),
                msg.getLocalAddresses(), publicAddr, System.currentTimeMillis()
        );
        fileTransferService.registerOffer(offer);

        log.info("[FILE] 收到文件 fileId={}, name={}, size={} bytes ({}), mime={}, from={}",
                msg.getFileId(), msg.getFileName(), msg.getFileSize(),
                humanReadableSize(msg.getFileSize()), msg.getMimeType(), cs.getDeviceId());

        Map<String, Object> notify = new java.util.LinkedHashMap<>();
        notify.put("type", "file_notify");
        notify.put("fileId", msg.getFileId());
        notify.put("fileName", msg.getFileName());
        notify.put("mimeType", msg.getMimeType());
        notify.put("fileSize", msg.getFileSize());
        notify.put("checksum", msg.getChecksum());
        notify.put("from", cs.getDeviceId());
        notify.put("sourceLocalAddresses", fileTransferLevel >= 1 ? (msg.getLocalAddresses() != null ? msg.getLocalAddresses() : List.of()) : List.of());
        notify.put("sourcePublicAddress", fileTransferLevel >= 2 ? publicAddr : "");
        notify.put("maxRelaySize", fileTransferLevel >= 3 ? fileTransferService.getMaxRelaySize() : 0);
        notify.put("udpPort", fileTransferLevel >= 2 ? udpPort : 0);
        notify.put("fileTransferLevel", fileTransferLevel);
        String json = gson.toJson(notify);
        var results = sessionManager.broadcastToUser(cs.getUsername(), session.getId(), json);
        int sent = (int) results.stream().filter(SessionManager.SendResult::success).count();
        log.info("[FILE] file_notify 通知完成, fileId={}, 目标{}个, 成功{}个",
                msg.getFileId(), results.size(), sent);
        for (var r : results) {
            if (r.success()) {
                log.info("[FILE]   -> device={}, 通知耗时{}ms", r.deviceId(), r.elapsedMs());
            } else {
                log.warn("[FILE]   -> device={}, 通知失败: {}", r.deviceId(), r.error());
            }
        }
    }

    private void handleFileRequest(WebSocketSession session, ClientSession cs, Message msg) throws Exception {
        if (!cs.isLoggedIn()) { sendResponse(session, "error", false, "Not logged in"); return; }
        var offer = fileTransferService.getOffer(msg.getFileId());
        if (offer == null) {
            sendResponse(session, "error", false, "File offer not found: " + msg.getFileId());
            return;
        }
        String receiverPublicAddr = session.getRemoteAddress() != null
                ? session.getRemoteAddress().getAddress().getHostAddress() : "unknown";

        boolean sameLan = receiverPublicAddr.equals(offer.senderPublicAddress())
                && !"unknown".equals(receiverPublicAddr);

        if (sameLan) {
            log.info("[FILE] 检测到同局域网传输 fileId={}, publicAddr={}", msg.getFileId(), receiverPublicAddr);
        }

        // Tell the source (sender) about this receiver
        Map<String, Object> peerInfoForSender = new java.util.LinkedHashMap<>();
        peerInfoForSender.put("type", "file_peer_info");
        peerInfoForSender.put("fileId", msg.getFileId());
        peerInfoForSender.put("peerId", cs.getDeviceId());
        peerInfoForSender.put("peerLocalAddresses", fileTransferLevel >= 1 ? (msg.getLocalAddresses() != null ? msg.getLocalAddresses() : List.of()) : List.of());
        peerInfoForSender.put("peerPublicAddress", sameLan ? "" : (fileTransferLevel >= 2 ? receiverPublicAddr : ""));
        peerInfoForSender.put("sameLan", sameLan);
        peerInfoForSender.put("role", "sender");
        peerInfoForSender.put("fileTransferLevel", fileTransferLevel);
        WebSocketSession senderSession = sessionManager.getWebSocketSession(offer.senderSessionId());
        if (senderSession != null && senderSession.isOpen()) {
            senderSession.sendMessage(new TextMessage(gson.toJson(peerInfoForSender)));
        }

        // Tell the receiver about the source
        Map<String, Object> peerInfoForReceiver = new java.util.LinkedHashMap<>();
        peerInfoForReceiver.put("type", "file_peer_info");
        peerInfoForReceiver.put("fileId", msg.getFileId());
        peerInfoForReceiver.put("peerId", offer.senderDeviceId());
        peerInfoForReceiver.put("peerLocalAddresses", fileTransferLevel >= 1 ? (offer.senderLocalAddresses() != null ? offer.senderLocalAddresses() : List.of()) : List.of());
        peerInfoForReceiver.put("peerPublicAddress", sameLan ? "" : (fileTransferLevel >= 2 ? offer.senderPublicAddress() : ""));
        peerInfoForReceiver.put("sameLan", sameLan);
        peerInfoForReceiver.put("role", "receiver");
        peerInfoForReceiver.put("fileTransferLevel", fileTransferLevel);
        session.sendMessage(new TextMessage(gson.toJson(peerInfoForReceiver)));
        log.info("[FILE] file_request 交换端点信息, fileId={}, sender={}, receiver={}, sameLan={}",
                msg.getFileId(), offer.senderDeviceId(), cs.getDeviceId(), sameLan);
    }

    private void handleFileRelay(WebSocketSession session, ClientSession cs, Message msg) throws Exception {
        if (!cs.isLoggedIn()) { sendResponse(session, "error", false, "Not logged in"); return; }
        if (fileTransferLevel < 3) {
            sendResponse(session, "error", false, "Server relay is not allowed (level=" + fileTransferLevel + ")");
            return;
        }
        var offer = fileTransferService.getOffer(msg.getFileId());
        if (offer == null) {
            sendResponse(session, "error", false, "File offer not found");
            return;
        }
        if (msg.getFileSize() > fileTransferService.getMaxRelaySize()) {
            sendResponse(session, "file_relay_result", false,
                    "File too large for relay: " + msg.getFileSize() + " > " + fileTransferService.getMaxRelaySize());
            return;
        }
        if (msg.getTargetDevice() != null) {
            Map<String, Object> relayData = Map.of(
                    "type", "file_relay_data",
                    "fileId", msg.getFileId(),
                    "fileName", offer.fileName(),
                    "mimeType", offer.mimeType(),
                    "fileSize", offer.fileSize(),
                    "checksum", offer.checksum(),
                    "data", msg.getData(),
                    "from", cs.getDeviceId()
            );
            int sent = sessionManager.sendToDevice(cs.getUsername(), msg.getTargetDevice(), gson.toJson(relayData));
            log.info("[FILE] relay 中转 fileId={}, 文件={}, 大小={} ({}) -> target={}, sent={}",
                    msg.getFileId(), offer.fileName(), offer.fileSize(),
                    humanReadableSize(offer.fileSize()), msg.getTargetDevice(), sent);
        } else {
            // Receiver is requesting relay — ask the source to send data
            Map<String, Object> relayReq = Map.of(
                    "type", "file_relay_request",
                    "fileId", msg.getFileId(),
                    "requesterId", cs.getDeviceId()
            );
            WebSocketSession senderSession = sessionManager.getWebSocketSession(offer.senderSessionId());
            if (senderSession != null && senderSession.isOpen()) {
                senderSession.sendMessage(new TextMessage(gson.toJson(relayReq)));
                log.info("[FILE] relay 请求已发送给 sender={}, fileId={}", offer.senderDeviceId(), msg.getFileId());
            }
        }
    }

    private void handleFileTransferResult(WebSocketSession session, ClientSession cs, Message msg) throws Exception {
        var offer = fileTransferService.getOffer(msg.getFileId());
        if (offer != null) {
            long elapsedMs = System.currentTimeMillis() - offer.timestamp();
            long connMs = msg.getConnectionMs();
            long xferMs = msg.getTransferMs();
            double speed = xferMs > 0 ? (double) offer.fileSize() / 1024 / 1024 / ((double) xferMs / 1000) : 0;
            log.info("[FILE] 传输完成 fileId={}, 文件={}, 大小={} ({}), 接收端={}, 方式={}, 成功={}, " +
                            "连接耗时{}ms, 传输耗时{}ms ({} MB/s), 总耗时{}ms",
                    msg.getFileId(), offer.fileName(), offer.fileSize(),
                    humanReadableSize(offer.fileSize()),
                    cs.getDeviceId(), msg.getMethod(), msg.isSuccess(),
                    connMs, xferMs, String.format("%.1f", speed), elapsedMs);
        } else {
            log.info("[FILE] 传输结果 fileId={}, device={}, method={}, success={} (offer已过期)",
                    msg.getFileId(), cs.getDeviceId(), msg.getMethod(), msg.isSuccess());
        }
    }

    private static String humanReadableSize(long bytes) {
        if (bytes < 1024) return bytes + "B";
        if (bytes < 1024 * 1024) return String.format("%.1fKB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format("%.1fMB", bytes / (1024.0 * 1024));
        return String.format("%.2fGB", bytes / (1024.0 * 1024 * 1024));
    }

    private record Response(String type, boolean success, String message) {}
    private record ClipboardBroadcast(String type, String content, String from) {}
}
