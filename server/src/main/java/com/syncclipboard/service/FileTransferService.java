package com.syncclipboard.service;

import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

@Service
public class FileTransferService {

    private static final Logger log = LoggerFactory.getLogger(FileTransferService.class);

    private static final long OFFER_TTL_MS = 120_000L;
    private static final long CLEANUP_INTERVAL_S = 60L;

    @Value("${syncclipboard.max-relay-size:307200}")
    private long maxRelaySize;

    public record FileOffer(
            String fileId, String fileName, String mimeType, long fileSize,
            String checksum, String senderSessionId, String senderDeviceId,
            String senderUsername, List<String> senderLocalAddresses,
            String senderPublicAddress, long timestamp
    ) {}

    private final Map<String, FileOffer> activeOffers = new ConcurrentHashMap<>();
    private final ScheduledExecutorService cleanupExecutor;

    public FileTransferService() {
        this.cleanupExecutor = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "ft-cleanup");
            t.setDaemon(true);
            return t;
        });
        cleanupExecutor.scheduleAtFixedRate(this::cleanupExpired,
                CLEANUP_INTERVAL_S, CLEANUP_INTERVAL_S, TimeUnit.SECONDS);
    }

    private void cleanupExpired() {
        long cutoff = System.currentTimeMillis() - OFFER_TTL_MS;
        int before = activeOffers.size();
        activeOffers.entrySet().removeIf(e -> e.getValue().timestamp() < cutoff);
        int removed = before - activeOffers.size();
        if (removed > 0) {
            log.debug("[FILE] cleanup removed {} stale offer(s)", removed);
        }
    }

    public void registerOffer(FileOffer offer) {
        activeOffers.put(offer.fileId(), offer);
        log.info("[FILE] 注册文件 fileId={}, name={}, size={}, from={}",
                offer.fileId(), offer.fileName(), offer.fileSize(), offer.senderDeviceId());
    }

    public FileOffer getOffer(String fileId) {
        return activeOffers.get(fileId);
    }

    public void removeOffer(String fileId) {
        activeOffers.remove(fileId);
    }

    public long getMaxRelaySize() {
        return maxRelaySize;
    }

    @PreDestroy
    public void shutdown() {
        log.info("[SHUTDOWN] stopping ft-cleanup, active offers={}", activeOffers.size());
        cleanupExecutor.shutdown();
        try {
            if (!cleanupExecutor.awaitTermination(2, TimeUnit.SECONDS)) {
                cleanupExecutor.shutdownNow();
            }
        } catch (InterruptedException e) {
            cleanupExecutor.shutdownNow();
            Thread.currentThread().interrupt();
        }
    }
}
