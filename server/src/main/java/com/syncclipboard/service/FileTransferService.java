package com.syncclipboard.service;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

@Service
public class FileTransferService {

    private static final Logger log = LoggerFactory.getLogger(FileTransferService.class);

    @Value("${syncclipboard.max-relay-size:307200}")
    private long maxRelaySize;

    public record FileOffer(
            String fileId, String fileName, String mimeType, long fileSize,
            String checksum, String senderSessionId, String senderDeviceId,
            String senderUsername, List<String> senderLocalAddresses,
            String senderPublicAddress, long timestamp
    ) {}

    private final Map<String, FileOffer> activeOffers = new ConcurrentHashMap<>();

    public FileTransferService() {
        Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "ft-cleanup");
            t.setDaemon(true);
            return t;
        }).scheduleAtFixedRate(() -> {
            long cutoff = System.currentTimeMillis() - 120_000;
            activeOffers.entrySet().removeIf(e -> e.getValue().timestamp() < cutoff);
        }, 60, 60, TimeUnit.SECONDS);
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
}
