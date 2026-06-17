package com.syncclipboard.ws;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.syncclipboard.model.ClientSession;
import com.syncclipboard.model.WsMessage;
import com.syncclipboard.service.ClipboardCacheService;
import com.syncclipboard.service.FileTransferService;
import com.syncclipboard.service.SessionManager;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;
import org.springframework.web.socket.TextMessage;
import org.springframework.web.socket.WebSocketSession;

import java.util.List;

/**
 * Routes parsed v2 messages to the appropriate handler. Keeps each branch
 * small and side-effect-isolated to make unit testing tractable.
 */
@Component
public class MessageDispatcher {

    private static final Logger log = LoggerFactory.getLogger(MessageDispatcher.class);

    private final ObjectMapper mapper;
    private final SessionManager sessionManager;
    private final ClipboardCacheService clipboardCache;
    private final FileTransferService fileTransferService;

    @Value("${syncclipboard.udp-port:8081}")
    private int udpPort;

    @Value("${syncclipboard.file-transfer-level:2}")
    private int fileTransferLevel;

    public MessageDispatcher(ObjectMapper mapper, SessionManager sessionManager,
                             ClipboardCacheService clipboardCache,
                             FileTransferService fileTransferService) {
        this.mapper = mapper;
        this.sessionManager = sessionManager;
        this.clipboardCache = clipboardCache;
        this.fileTransferService = fileTransferService;
    }

    public void dispatch(WebSocketSession session, WsMessage msg) throws Exception {
        ClientSession cs = sessionManager.getSession(session.getId());
        if (cs == null) {
            session.close();
            return;
        }
        if (msg instanceof WsMessage.Hello hello) {
            log.debug("[V2] hello from device={}, version={}", hello.deviceId(), hello.appVersion());
        } else if (msg instanceof WsMessage.Clipboard clip) {
            handleClipboard(session, cs, clip);
        } else if (msg instanceof WsMessage.FileOffer offer) {
            handleFileOffer(session, cs, offer);
        } else if (msg instanceof WsMessage.FileRequest req) {
            handleFileRequest(session, cs, req);
        } else if (msg instanceof WsMessage.FileRelay relay) {
            handleFileRelay(session, cs, relay);
        } else if (msg instanceof WsMessage.FileTransferResult result) {
            handleResult(cs, result);
        } else if (msg instanceof WsMessage.Ping ping) {
            sendJson(session, new WsMessage.Pong("pong", ping.ts()));
        } else {
            log.warn("[V2] unexpected client message type={}", msg.type());
        }
    }

    private void handleClipboard(WebSocketSession session, ClientSession cs, WsMessage.Clipboard msg) {
        long start = System.currentTimeMillis();
        int contentLen = msg.content() != null ? msg.content().length() : 0;
        log.info("[V2-CLIP] user={}, device={}, len={}", cs.getUsername(), cs.getDeviceId(), contentLen);

        // Build the broadcast envelope using the same v2 schema (server-originated `from`).
        WsMessage.Clipboard broadcast = new WsMessage.Clipboard("clipboard", msg.msgId(), msg.content(), cs.getDeviceId());
        try {
            String json = mapper.writeValueAsString(broadcast);
            clipboardCache.store(cs.getUsername(), msg.content(), cs.getDeviceId());
            var results = sessionManager.broadcastToUser(cs.getUsername(), session.getId(), json);
            int sent = (int) results.stream().filter(SessionManager.SendResult::success).count();
            log.info("[V2-CLIP] sync done user={}, targets={}, ok={}, total={}ms",
                    cs.getUsername(), results.size(), sent, System.currentTimeMillis() - start);
        } catch (Exception e) {
            log.error("[V2-CLIP] broadcast failed", e);
        }
    }

    private void handleFileOffer(WebSocketSession session, ClientSession cs, WsMessage.FileOffer msg) throws Exception {
        if (fileTransferLevel <= 0) {
            sendJson(session, new WsMessage.Error("error", "FILE_TRANSFER_DISABLED",
                    "File transfer is disabled on server", false));
            return;
        }
        String publicAddr = session.getRemoteAddress() != null
                ? session.getRemoteAddress().getAddress().getHostAddress() : "unknown";
        var offer = new FileTransferService.FileOffer(
                msg.fileId(), msg.fileName(), msg.mimeType(), msg.fileSize(),
                msg.checksum(), session.getId(), cs.getDeviceId(), cs.getUsername(),
                msg.localAddresses(), publicAddr, System.currentTimeMillis());
        fileTransferService.registerOffer(offer);

        WsMessage.FileNotify notify = new WsMessage.FileNotify(
                "file_notify", msg.fileId(), msg.fileName(), msg.mimeType(),
                msg.fileSize(), msg.checksum(), cs.getDeviceId(),
                fileTransferLevel >= 1 ? safe(msg.localAddresses()) : List.of(),
                fileTransferLevel >= 2 ? publicAddr : "",
                fileTransferLevel >= 3 ? fileTransferService.getMaxRelaySize() : 0,
                fileTransferLevel >= 2 ? udpPort : 0,
                fileTransferLevel);
        String json = mapper.writeValueAsString(notify);
        var results = sessionManager.broadcastToUser(cs.getUsername(), session.getId(), json);
        int sent = (int) results.stream().filter(SessionManager.SendResult::success).count();
        log.info("[V2-OFFER] fileId={}, targets={}, ok={}", msg.fileId(), results.size(), sent);
    }

    private void handleFileRequest(WebSocketSession session, ClientSession cs, WsMessage.FileRequest msg) throws Exception {
        var offer = fileTransferService.getOffer(msg.fileId());
        if (offer == null) {
            sendJson(session, new WsMessage.Error("error", "FILE_OFFER_NOT_FOUND",
                    "File offer not found: " + msg.fileId(), false));
            return;
        }
        String receiverPublicAddr = session.getRemoteAddress() != null
                ? session.getRemoteAddress().getAddress().getHostAddress() : "unknown";
        boolean sameLan = receiverPublicAddr.equals(offer.senderPublicAddress())
                && !"unknown".equals(receiverPublicAddr);

        WsMessage.FilePeerInfo forSender = new WsMessage.FilePeerInfo(
                "file_peer_info", msg.fileId(), cs.getDeviceId(),
                fileTransferLevel >= 1 ? safe(msg.localAddresses()) : List.of(),
                sameLan ? "" : (fileTransferLevel >= 2 ? receiverPublicAddr : ""),
                sameLan, "sender", fileTransferLevel);
        WebSocketSession senderSession = sessionManager.getWebSocketSession(offer.senderSessionId());
        if (senderSession != null && senderSession.isOpen()) {
            senderSession.sendMessage(new TextMessage(mapper.writeValueAsString(forSender)));
        }

        WsMessage.FilePeerInfo forReceiver = new WsMessage.FilePeerInfo(
                "file_peer_info", msg.fileId(), offer.senderDeviceId(),
                fileTransferLevel >= 1 ? safe(offer.senderLocalAddresses()) : List.of(),
                sameLan ? "" : (fileTransferLevel >= 2 ? offer.senderPublicAddress() : ""),
                sameLan, "receiver", fileTransferLevel);
        sendJson(session, forReceiver);
        log.info("[V2-REQ] fileId={}, sameLan={}", msg.fileId(), sameLan);
    }

    private void handleFileRelay(WebSocketSession session, ClientSession cs, WsMessage.FileRelay msg) throws Exception {
        if (fileTransferLevel < 3) {
            sendJson(session, new WsMessage.Error("error", "FILE_TRANSFER_DISABLED",
                    "Server relay disabled (level=" + fileTransferLevel + ")", false));
            return;
        }
        var offer = fileTransferService.getOffer(msg.fileId());
        if (offer == null) {
            sendJson(session, new WsMessage.Error("error", "FILE_OFFER_NOT_FOUND",
                    "File offer not found", false));
            return;
        }
        if (msg.fileSize() > fileTransferService.getMaxRelaySize()) {
            sendJson(session, new WsMessage.Error("error", "FILE_TOO_LARGE",
                    "File too large for relay: " + msg.fileSize() + " > " + fileTransferService.getMaxRelaySize(),
                    false));
            return;
        }
        if (msg.targetDevice() != null) {
            WsMessage.FileRelayData relayData = new WsMessage.FileRelayData(
                    "file_relay_data", msg.fileId(), offer.fileName(), offer.mimeType(),
                    offer.fileSize(), offer.checksum(), msg.data(), cs.getDeviceId());
            int sent = sessionManager.sendToDevice(cs.getUsername(), msg.targetDevice(),
                    mapper.writeValueAsString(relayData));
            log.info("[V2-RELAY] fileId={} -> target={}, sent={}", msg.fileId(), msg.targetDevice(), sent);
        } else {
            WsMessage.FileRelayRequest req = new WsMessage.FileRelayRequest(
                    "file_relay_request", msg.fileId(), cs.getDeviceId());
            WebSocketSession senderSession = sessionManager.getWebSocketSession(offer.senderSessionId());
            if (senderSession != null && senderSession.isOpen()) {
                senderSession.sendMessage(new TextMessage(mapper.writeValueAsString(req)));
                log.info("[V2-RELAY] requested data from sender={}, fileId={}",
                        offer.senderDeviceId(), msg.fileId());
            }
        }
    }

    private void handleResult(ClientSession cs, WsMessage.FileTransferResult msg) {
        var offer = fileTransferService.getOffer(msg.fileId());
        if (offer != null) {
            log.info("[V2-RESULT] fileId={}, name={}, size={}, device={}, method={}, success={}, conn={}ms, xfer={}ms",
                    msg.fileId(), offer.fileName(), offer.fileSize(),
                    cs.getDeviceId(), msg.method(), msg.success(),
                    msg.connectionMs(), msg.transferMs());
            fileTransferService.removeOffer(msg.fileId());
        } else {
            log.info("[V2-RESULT] fileId={} (offer expired) device={}, method={}, success={}",
                    msg.fileId(), cs.getDeviceId(), msg.method(), msg.success());
        }
    }

    private void sendJson(WebSocketSession session, WsMessage payload) throws Exception {
        if (session.isOpen()) {
            session.sendMessage(new TextMessage(mapper.writeValueAsString(payload)));
        }
    }

    private static <T> List<T> safe(List<T> in) {
        return in == null ? List.of() : in;
    }
}
