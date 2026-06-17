package com.syncclipboard.model;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonSubTypes;
import com.fasterxml.jackson.annotation.JsonTypeInfo;

import java.util.List;

/**
 * Sealed protocol-v2 message hierarchy. Polymorphic on the {@code type} field
 * via Jackson. All subclasses are immutable records and may include a
 * protocol version, message id, and timestamp from the v2 schema.
 */
@JsonTypeInfo(use = JsonTypeInfo.Id.NAME, property = "type", visible = true)
@JsonSubTypes({
        @JsonSubTypes.Type(value = WsMessage.Hello.class, name = "hello"),
        @JsonSubTypes.Type(value = WsMessage.HelloAck.class, name = "hello_ack"),
        @JsonSubTypes.Type(value = WsMessage.Clipboard.class, name = "clipboard"),
        @JsonSubTypes.Type(value = WsMessage.FileOffer.class, name = "file_offer"),
        @JsonSubTypes.Type(value = WsMessage.FileNotify.class, name = "file_notify"),
        @JsonSubTypes.Type(value = WsMessage.FileRequest.class, name = "file_request"),
        @JsonSubTypes.Type(value = WsMessage.FilePeerInfo.class, name = "file_peer_info"),
        @JsonSubTypes.Type(value = WsMessage.FileRelay.class, name = "file_relay"),
        @JsonSubTypes.Type(value = WsMessage.FileRelayData.class, name = "file_relay_data"),
        @JsonSubTypes.Type(value = WsMessage.FileRelayRequest.class, name = "file_relay_request"),
        @JsonSubTypes.Type(value = WsMessage.FileTransferResult.class, name = "file_transfer_result"),
        @JsonSubTypes.Type(value = WsMessage.Error.class, name = "error"),
        @JsonSubTypes.Type(value = WsMessage.Ping.class, name = "ping"),
        @JsonSubTypes.Type(value = WsMessage.Pong.class, name = "pong"),
})
@JsonIgnoreProperties(ignoreUnknown = true)
@JsonInclude(JsonInclude.Include.NON_NULL)
public sealed interface WsMessage {

    String type();

    record Hello(String type, int v, String clientType, String deviceId, String appVersion,
                 List<String> capabilities) implements WsMessage {}

    record HelloAck(String type, int v, List<String> accepted, int serverFileLevel,
                    int udpPort, long maxRelaySize, long maxClipboardBytes) implements WsMessage {}

    record Clipboard(String type, String msgId, String content, String from) implements WsMessage {}

    record FileOffer(String type, String msgId, String fileId, String fileName,
                     String mimeType, long fileSize, String checksum,
                     List<String> localAddresses) implements WsMessage {}

    record FileNotify(String type, String fileId, String fileName, String mimeType,
                      long fileSize, String checksum, String from,
                      List<String> sourceLocalAddresses, String sourcePublicAddress,
                      long maxRelaySize, int udpPort, int fileTransferLevel) implements WsMessage {}

    record FileRequest(String type, String msgId, String fileId,
                       List<String> localAddresses) implements WsMessage {}

    record FilePeerInfo(String type, String fileId, String peerId,
                        List<String> peerLocalAddresses, String peerPublicAddress,
                        boolean sameLan, String role, int fileTransferLevel) implements WsMessage {}

    record FileRelay(String type, String msgId, String fileId, long fileSize,
                     String targetDevice, String data) implements WsMessage {}

    record FileRelayData(String type, String fileId, String fileName, String mimeType,
                         long fileSize, String checksum, String data, String from) implements WsMessage {}

    record FileRelayRequest(String type, String fileId, String requesterId) implements WsMessage {}

    record FileTransferResult(String type, String fileId, String method, boolean success,
                              long connectionMs, long transferMs) implements WsMessage {}

    record Error(String type, String code, String message, boolean retryable) implements WsMessage {}

    record Ping(String type, long ts) implements WsMessage {}
    record Pong(String type, long ts) implements WsMessage {}
}
