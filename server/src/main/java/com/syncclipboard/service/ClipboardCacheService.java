package com.syncclipboard.service;

import com.google.gson.Gson;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

@Service
public class ClipboardCacheService {

    private static final Logger log = LoggerFactory.getLogger(ClipboardCacheService.class);
    private final Gson gson = new Gson();

    private record CachedClipboard(String encryptedContent, String fromDevice, long timestamp) {}

    private final Map<String, CachedClipboard> cache = new ConcurrentHashMap<>();

    public void store(String username, String encryptedContent, String fromDevice) {
        cache.put(username, new CachedClipboard(encryptedContent, fromDevice, System.currentTimeMillis()));
        log.info("[CACHE] 缓存剪切板 user={}, from={}, len={}", username, fromDevice, encryptedContent.length());
    }

    /**
     * 获取用户最新的剪切板缓存，排除来自指定设备的内容（避免发回给发送者）
     */
    public String getForDevice(String username, String deviceId) {
        CachedClipboard cached = cache.get(username);
        if (cached == null) return null;
        if (cached.fromDevice.equals(deviceId)) return null;
        java.util.Map<String, Object> msg = new java.util.LinkedHashMap<>();
        msg.put("type", "clipboard");
        msg.put("content", cached.encryptedContent());
        msg.put("from", cached.fromDevice());
        return gson.toJson(msg);
    }
}
