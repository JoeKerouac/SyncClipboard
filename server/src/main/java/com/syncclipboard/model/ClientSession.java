package com.syncclipboard.model;

import org.springframework.web.socket.WebSocketSession;
import org.springframework.web.socket.handler.ConcurrentWebSocketSessionDecorator;

public class ClientSession {
    private final WebSocketSession session;
    private boolean authenticated;
    private boolean loggedIn;
    private String username;
    private String deviceId;

    public ClientSession(WebSocketSession session) {
        this.session = new ConcurrentWebSocketSessionDecorator(session, 5000, 2 * 1024 * 1024);
        this.authenticated = false;
        this.loggedIn = false;
    }

    public WebSocketSession getSession() { return session; }

    public boolean isAuthenticated() { return authenticated; }
    public void setAuthenticated(boolean authenticated) { this.authenticated = authenticated; }

    public boolean isLoggedIn() { return loggedIn; }
    public void setLoggedIn(boolean loggedIn) { this.loggedIn = loggedIn; }

    public String getUsername() { return username; }
    public void setUsername(String username) { this.username = username; }

    public String getDeviceId() { return deviceId; }
    public void setDeviceId(String deviceId) { this.deviceId = deviceId; }
}
