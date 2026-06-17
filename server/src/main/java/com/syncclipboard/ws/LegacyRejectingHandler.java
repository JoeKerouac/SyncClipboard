package com.syncclipboard.ws;

import org.springframework.web.socket.CloseStatus;
import org.springframework.web.socket.WebSocketSession;
import org.springframework.web.socket.handler.TextWebSocketHandler;

/**
 * Closes any client that connects to the v1 path with code 1003 (unsupported
 * data) so they get a definitive signal to upgrade. Stage B of the v2.0
 * rollout is breaking — there is no compatibility shim.
 */
public class LegacyRejectingHandler extends TextWebSocketHandler {

    private static final CloseStatus UPGRADE_REQUIRED =
            new CloseStatus(1003, "protocol upgrade required (v2)");

    @Override
    public void afterConnectionEstablished(WebSocketSession session) throws Exception {
        session.close(UPGRADE_REQUIRED);
    }
}
