package com.syncclipboard.security;

import com.github.benmanes.caffeine.cache.Cache;
import com.github.benmanes.caffeine.cache.Caffeine;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.time.Duration;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Per-(username + remoteIp) login attempt limiter. Counts only failed attempts
 * within the configured rolling window. Threadsafe.
 */
@Component
public class LoginRateLimiter {

    private final int maxAttempts;
    private final Cache<String, AtomicInteger> counters;

    public LoginRateLimiter(@Value("${syncclipboard.login.max-attempts:5}") int maxAttempts,
                            @Value("${syncclipboard.login.window-seconds:60}") long windowSeconds) {
        this.maxAttempts = maxAttempts;
        this.counters = Caffeine.newBuilder()
                .expireAfterWrite(Duration.ofSeconds(windowSeconds))
                .maximumSize(10_000)
                .build();
    }

    /** Returns true if the caller is allowed to attempt a login right now. */
    public boolean tryAcquire(String username, String remoteIp) {
        String key = key(username, remoteIp);
        AtomicInteger c = counters.get(key, k -> new AtomicInteger(0));
        return c != null && c.get() < maxAttempts;
    }

    /** Record a failed attempt; counts toward the limit. */
    public void recordFailure(String username, String remoteIp) {
        String key = key(username, remoteIp);
        AtomicInteger c = counters.get(key, k -> new AtomicInteger(0));
        if (c != null) c.incrementAndGet();
    }

    /** Wipe the counter for a successful login so the user is not penalised. */
    public void recordSuccess(String username, String remoteIp) {
        counters.invalidate(key(username, remoteIp));
    }

    private static String key(String username, String remoteIp) {
        return (username == null ? "?" : username) + "@" + (remoteIp == null ? "?" : remoteIp);
    }
}
