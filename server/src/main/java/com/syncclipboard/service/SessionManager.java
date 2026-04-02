package com.syncclipboard.service;

import com.syncclipboard.model.ClientSession;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;
import org.springframework.web.socket.TextMessage;
import org.springframework.web.socket.WebSocketSession;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.stream.Collectors;

@Service
public class SessionManager {

    private static final Logger log = LoggerFactory.getLogger(SessionManager.class);

    private final ExecutorService broadcastPool = Executors.newFixedThreadPool(
            Runtime.getRuntime().availableProcessors(),
            r -> { Thread t = new Thread(r, "broadcast"); t.setDaemon(true); return t; });

    private final Map<String, ClientSession> sessions = new ConcurrentHashMap<>();

    public void addSession(WebSocketSession session) {
        sessions.put(session.getId(), new ClientSession(session));
    }

    public void removeSession(String sessionId) {
        sessions.remove(sessionId);
    }

    public ClientSession getSession(String sessionId) {
        return sessions.get(sessionId);
    }

    public int getOnlineCount() {
        return sessions.size();
    }

    public record SendResult(String deviceId, String sessionId, boolean success, long elapsedMs, String error) {}

    /**
     * 向同一用户的其他在线客户端广播消息，返回每个设备的发送结果(含耗时)
     */
    public List<SendResult> broadcastToUser(String username, String senderSessionId, String message) {
        List<ClientSession> targets = sessions.values().stream()
                .filter(s -> s.isLoggedIn()
                        && username.equals(s.getUsername())
                        && !s.getSession().getId().equals(senderSessionId))
                .collect(Collectors.toList());

        log.debug("[BROADCAST] user={}, 排除发送者后目标数={}", username, targets.size());

        List<CompletableFuture<SendResult>> futures = new ArrayList<>();
        for (ClientSession target : targets) {
            futures.add(CompletableFuture.supplyAsync(() -> {
                long start = System.currentTimeMillis();
                try {
                    if (target.getSession().isOpen()) {
                        target.getSession().sendMessage(new TextMessage(message));
                        long elapsed = System.currentTimeMillis() - start;
                        return new SendResult(target.getDeviceId(), target.getSession().getId(), true, elapsed, null);
                    } else {
                        return new SendResult(target.getDeviceId(), target.getSession().getId(), false, 0, "session closed");
                    }
                } catch (IOException e) {
                    long elapsed = System.currentTimeMillis() - start;
                    return new SendResult(target.getDeviceId(), target.getSession().getId(), false, elapsed, e.getMessage());
                }
            }, broadcastPool));
        }

        return futures.stream()
                .map(CompletableFuture::join)
                .collect(Collectors.toList());
    }

    public WebSocketSession getWebSocketSession(String sessionId) {
        ClientSession cs = sessions.get(sessionId);
        return cs != null ? cs.getSession() : null;
    }

    public int sendToDevice(String username, String targetDeviceId, String message) {
        int sent = 0;
        for (ClientSession cs : sessions.values()) {
            if (cs.isLoggedIn() && username.equals(cs.getUsername())
                    && targetDeviceId.equals(cs.getDeviceId()) && cs.getSession().isOpen()) {
                try {
                    cs.getSession().sendMessage(new TextMessage(message));
                    sent++;
                } catch (IOException e) {
                    log.error("[SEND] 发送到设备失败 device={}, error={}", targetDeviceId, e.getMessage());
                }
            }
        }
        return sent;
    }
}
