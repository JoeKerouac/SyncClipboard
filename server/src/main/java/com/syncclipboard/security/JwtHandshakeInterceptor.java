package com.syncclipboard.security;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.HttpStatus;
import org.springframework.http.server.ServerHttpRequest;
import org.springframework.http.server.ServerHttpResponse;
import org.springframework.http.server.ServletServerHttpRequest;
import org.springframework.http.server.ServletServerHttpResponse;
import org.springframework.stereotype.Component;
import org.springframework.web.socket.WebSocketHandler;
import org.springframework.web.socket.server.HandshakeInterceptor;

import java.util.Map;

/**
 * Authenticates WebSocket clients during the HTTP handshake. Clients are
 * expected to advertise a single subprotocol of the form
 * {@code bearer.<jwt>}. On success, the parsed claims are added to the
 * session attributes (keys: {@code AUTH_USERNAME}, {@code AUTH_DEVICE_ID}).
 */
@Component
public class JwtHandshakeInterceptor implements HandshakeInterceptor {

    public static final String ATTR_USERNAME = "AUTH_USERNAME";
    public static final String ATTR_DEVICE_ID = "AUTH_DEVICE_ID";

    private static final Logger log = LoggerFactory.getLogger(JwtHandshakeInterceptor.class);
    private static final String BEARER_PREFIX = "bearer.";
    private static final String SEC_WEBSOCKET_PROTOCOL = "Sec-WebSocket-Protocol";

    private final JwtService jwtService;

    public JwtHandshakeInterceptor(JwtService jwtService) {
        this.jwtService = jwtService;
    }

    @Override
    public boolean beforeHandshake(ServerHttpRequest request, ServerHttpResponse response,
                                   WebSocketHandler wsHandler, Map<String, Object> attributes) {
        String token = extractToken(request);
        if (token == null) {
            reject(response, "missing bearer token");
            return false;
        }
        JwtService.ParsedToken parsed = jwtService.verifyAccessToken(token);
        if (parsed == null) {
            reject(response, "invalid or expired token");
            return false;
        }
        attributes.put(ATTR_USERNAME, parsed.username());
        attributes.put(ATTR_DEVICE_ID, parsed.deviceId());
        // Echo the protocol back so the client's negotiation succeeds.
        response.getHeaders().set(SEC_WEBSOCKET_PROTOCOL, BEARER_PREFIX + token);
        return true;
    }

    @Override
    public void afterHandshake(ServerHttpRequest request, ServerHttpResponse response,
                               WebSocketHandler wsHandler, Exception ex) {
        // no-op
    }

    private String extractToken(ServerHttpRequest request) {
        if (request instanceof ServletServerHttpRequest servletRequest) {
            String headerValues = servletRequest.getServletRequest().getHeader(SEC_WEBSOCKET_PROTOCOL);
            if (headerValues != null) {
                for (String entry : headerValues.split(",")) {
                    String trimmed = entry.trim();
                    if (trimmed.startsWith(BEARER_PREFIX)) {
                        return trimmed.substring(BEARER_PREFIX.length());
                    }
                }
            }
        }
        return null;
    }

    private void reject(ServerHttpResponse response, String reason) {
        log.warn("[WS-HANDSHAKE] rejected: {}", reason);
        if (response instanceof ServletServerHttpResponse servletResponse) {
            servletResponse.setStatusCode(HttpStatus.UNAUTHORIZED);
        }
    }
}
