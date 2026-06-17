package com.syncclipboard.config;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.boot.context.properties.bind.DefaultValue;

import java.time.Duration;
import java.util.List;

/**
 * Centralised configuration for SyncClipboard server. Backed by
 * {@code syncclipboard.*} keys in {@code application.properties}. Replaces
 * scattered {@code @Value} usages.
 */
@ConfigurationProperties(prefix = "syncclipboard")
public record AppProperties(
        @DefaultValue("data") String dataDir,
        @DefaultValue("data/users.properties") String usersFile,
        @DefaultValue("") String serverKey,
        @DefaultValue Auth auth,
        @DefaultValue Login login,
        @DefaultValue("307200") long maxRelaySize,
        @DefaultValue("8081") int udpPort,
        @DefaultValue("2") int fileTransferLevel,
        @DefaultValue Origins origins
) {
    public record Auth(
            @DefaultValue("PT24H") Duration accessTtl,
            @DefaultValue("P30D") Duration refreshTtl
    ) {}

    public record Login(
            @DefaultValue("5") int maxAttempts,
            @DefaultValue("60") long windowSeconds
    ) {}

    public record Origins(
            @DefaultValue List<String> allowedPatterns
    ) {}
}
