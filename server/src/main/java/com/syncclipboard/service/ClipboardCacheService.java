package com.syncclipboard.service;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

@Service
public class ClipboardCacheService {

    private static final Logger log = LoggerFactory.getLogger(ClipboardCacheService.class);

    private static final long TTL_MS = 60L * 60L * 1000L;
    private static final long EVICT_INTERVAL_S = 5L * 60L;

    private final ObjectMapper mapper;
    private final ScheduledExecutorService evictor;

    public ClipboardCacheService(ObjectMapper mapper) {
        this.mapper = mapper;
        this.evictor = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "clip-cache-evict");
            t.setDaemon(true);
            return t;
        });
        evictor.scheduleAtFixedRate(this::evictExpired, EVICT_INTERVAL_S, EVICT_INTERVAL_S, TimeUnit.SECONDS);
    }

    private record CachedClipboard(String encryptedContent, String fromDevice, long timestamp) {}

    private final Map<String, CachedClipboard> cache = new ConcurrentHashMap<>();

    public void store(String username, String encryptedContent, String fromDevice) {
        cache.put(username, new CachedClipboard(encryptedContent, fromDevice, System.currentTimeMillis()));
        log.info("[CACHE] 缓存剪切板 user={}, from={}, len={}", username, fromDevice,
                encryptedContent != null ? encryptedContent.length() : 0);
    }

    /**
     * 获取用户最新的剪切板缓存，排除来自指定设备的内容（避免发回给发送者）。
     */
    public String getForDevice(String username, String deviceId) {
        CachedClipboard cached = cache.get(username);
        if (cached == null) return null;
        if (cached.fromDevice.equals(deviceId)) return null;
        if (System.currentTimeMillis() - cached.timestamp > TTL_MS) {
            cache.remove(username, cached);
            return null;
        }
        Map<String, Object> msg = new LinkedHashMap<>();
        msg.put("type", "clipboard");
        msg.put("content", cached.encryptedContent());
        msg.put("from", cached.fromDevice());
        try {
            return mapper.writeValueAsString(msg);
        } catch (JsonProcessingException e) {
            log.error("[CACHE] serialise failed", e);
            return null;
        }
    }

    private void evictExpired() {
        long cutoff = System.currentTimeMillis() - TTL_MS;
        int before = cache.size();
        cache.entrySet().removeIf(e -> e.getValue().timestamp() < cutoff);
        int removed = before - cache.size();
        if (removed > 0) log.debug("[CACHE] evicted {} stale entries", removed);
    }

    @PreDestroy
    public void shutdown() {
        evictor.shutdown();
        try {
            if (!evictor.awaitTermination(2, TimeUnit.SECONDS)) {
                evictor.shutdownNow();
            }
        } catch (InterruptedException e) {
            evictor.shutdownNow();
            Thread.currentThread().interrupt();
        }
    }
}
