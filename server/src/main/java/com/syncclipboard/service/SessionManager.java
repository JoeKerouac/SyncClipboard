package com.syncclipboard.service;

import com.syncclipboard.model.ClientSession;
import jakarta.annotation.PreDestroy;
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
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;

@Service
public class SessionManager {

    private static final Logger log = LoggerFactory.getLogger(SessionManager.class);
    private static final int MAX_SESSIONS_PER_USER = 10;

    private final ExecutorService broadcastPool = Executors.newFixedThreadPool(
            Runtime.getRuntime().availableProcessors(),
            r -> { Thread t = new Thread(r, "broadcast"); t.setDaemon(true); return t; });

    private final Map<String, ClientSession> sessions = new ConcurrentHashMap<>();

    public void addSession(WebSocketSession session) {
        sessions.put(session.getId(), new ClientSession(session));
    }

    public boolean addAuthenticated(WebSocketSession session, String username, String deviceId) {
        long count = sessions.values().stream()
                .filter(s -> s.isLoggedIn() && username.equals(s.getUsername()))
                .count();
        if (count >= MAX_SESSIONS_PER_USER) {
            log.warn("[SESSION] user={} exceeded max sessions ({}), rejecting", username, MAX_SESSIONS_PER_USER);
            return false;
        }
        ClientSession cs = new ClientSession(session);
        cs.setAuthenticated(true);
        cs.setLoggedIn(true);
        cs.setUsername(username);
        cs.setDeviceId(deviceId);
        sessions.put(session.getId(), cs);
        return true;
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

    public List<SendResult> broadcastToUser(String username, String senderSessionId, String message) {
        List<ClientSession> targets = sessions.values().stream()
                .filter(s -> s.isLoggedIn()
                        && username.equals(s.getUsername())
                        && !s.getSession().getId().equals(senderSessionId))
                .collect(Collectors.toList());

        log.debug("[BROADCAST] user={}, targets={}", username, targets.size());

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
                        sessions.remove(target.getSession().getId());
                        return new SendResult(target.getDeviceId(), target.getSession().getId(), false, 0, "session closed");
                    }
                } catch (IOException e) {
                    long elapsed = System.currentTimeMillis() - start;
                    sessions.remove(target.getSession().getId());
                    return new SendResult(target.getDeviceId(), target.getSession().getId(), false, elapsed, e.getMessage());
                }
            }, broadcastPool));
        }

        return futures.stream()
                .map(f -> {
                    try {
                        return f.orTimeout(5, TimeUnit.SECONDS).join();
                    } catch (Exception e) {
                        return new SendResult("?", "?", false, 0, "timeout: " + e.getMessage());
                    }
                })
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
                    log.error("[SEND] failed to send to device={}, error={}", targetDeviceId, e.getMessage());
                }
            }
        }
        return sent;
    }

    @PreDestroy
    public void shutdown() {
        log.info("[SHUTDOWN] closing broadcastPool, pending sessions={}", sessions.size());
        broadcastPool.shutdown();
        try {
            if (!broadcastPool.awaitTermination(5, TimeUnit.SECONDS)) {
                broadcastPool.shutdownNow();
            }
        } catch (InterruptedException e) {
            broadcastPool.shutdownNow();
            Thread.currentThread().interrupt();
        }
    }
}
