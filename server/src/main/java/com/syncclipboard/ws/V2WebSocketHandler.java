package com.syncclipboard.ws;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.syncclipboard.model.WsMessage;
import com.syncclipboard.security.JwtHandshakeInterceptor;
import com.syncclipboard.service.ClipboardCacheService;
import com.syncclipboard.service.FileTransferService;
import com.syncclipboard.service.SessionManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;
import org.springframework.web.socket.CloseStatus;
import org.springframework.web.socket.TextMessage;
import org.springframework.web.socket.WebSocketSession;
import org.springframework.web.socket.handler.TextWebSocketHandler;

import java.util.List;
import java.util.Map;

/**
 * Protocol v2 handler. Authentication is performed during the HTTP handshake
 * by {@link com.syncclipboard.security.JwtHandshakeInterceptor}; by the time
 * a session reaches this class it is already authenticated.
 */
@Component
public class V2WebSocketHandler extends TextWebSocketHandler {

    private static final Logger log = LoggerFactory.getLogger(V2WebSocketHandler.class);
    private static final int PROTO_VERSION = 2;

    private final ObjectMapper mapper;
    private final SessionManager sessionManager;
    private final ClipboardCacheService clipboardCache;
    private final FileTransferService fileTransferService;
    private final MessageDispatcher dispatcher;

    @Value("${syncclipboard.udp-port:8081}")
    private int udpPort;

    @Value("${syncclipboard.file-transfer-level:2}")
    private int fileTransferLevel;

    public V2WebSocketHandler(ObjectMapper mapper,
                              SessionManager sessionManager,
                              ClipboardCacheService clipboardCache,
                              FileTransferService fileTransferService,
                              MessageDispatcher dispatcher) {
        this.mapper = mapper;
        this.sessionManager = sessionManager;
        this.clipboardCache = clipboardCache;
        this.fileTransferService = fileTransferService;
        this.dispatcher = dispatcher;
    }

    @Override
    public void afterConnectionEstablished(WebSocketSession session) throws Exception {
        Map<String, Object> attrs = session.getAttributes();
        String username = (String) attrs.get(JwtHandshakeInterceptor.ATTR_USERNAME);
        String deviceId = (String) attrs.get(JwtHandshakeInterceptor.ATTR_DEVICE_ID);
        if (username == null || deviceId == null) {
            session.close(new CloseStatus(1011, "auth attrs missing"));
            return;
        }
        if (!sessionManager.addAuthenticated(session, username, deviceId)) {
            session.close(new CloseStatus(1008, "too many sessions"));
            return;
        }
        log.info("[V2-CONNECT] sessionId={}, user={}, device={}", session.getId(), username, deviceId);
        // Send a hello_ack so the client knows server capabilities.
        WsMessage.HelloAck ack = new WsMessage.HelloAck(
                "hello_ack", PROTO_VERSION,
                List.of("clipboard", "file_lan", "file_nat", "file_relay"),
                fileTransferLevel, udpPort,
                fileTransferService.getMaxRelaySize(),
                /* maxClipboardBytes = */ 1024L * 1024L);
        send(session, ack);

        // Replay cached clipboard, if any.
        String cached = clipboardCache.getForDevice(username, deviceId);
        if (cached != null) {
            session.sendMessage(new TextMessage(cached));
        }
    }

    @Override
    protected void handleTextMessage(WebSocketSession session, TextMessage textMessage) throws Exception {
        String payload = textMessage.getPayload();
        log.debug("[V2-RECV] sessionId={}, len={}", session.getId(), payload.length());
        WsMessage msg;
        try {
            msg = mapper.readValue(payload, WsMessage.class);
        } catch (Exception e) {
            sendError(session, "MSG_INVALID", "Invalid message format", false);
            return;
        }
        try {
            dispatcher.dispatch(session, msg);
        } catch (RuntimeException e) {
            log.error("[V2-DISPATCH] {} threw", msg.type(), e);
            sendError(session, "INTERNAL_ERROR", "Server error handling " + msg.type(), true);
        }
    }

    @Override
    public void afterConnectionClosed(WebSocketSession session, CloseStatus status) {
        log.info("[V2-DISCONNECT] sessionId={}, status={}", session.getId(), status);
        sessionManager.removeSession(session.getId());
    }

    @Override
    public void handleTransportError(WebSocketSession session, Throwable exception) {
        log.warn("[V2-ERROR] sessionId={}, err={}", session.getId(), exception.toString());
        sessionManager.removeSession(session.getId());
    }

    void send(WebSocketSession session, WsMessage msg) throws Exception {
        if (session.isOpen()) {
            session.sendMessage(new TextMessage(mapper.writeValueAsString(msg)));
        }
    }

    void sendError(WebSocketSession session, String code, String message, boolean retryable) throws Exception {
        send(session, new WsMessage.Error("error", code, message, retryable));
    }
}
