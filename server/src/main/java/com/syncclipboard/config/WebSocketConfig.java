package com.syncclipboard.config;

import com.syncclipboard.security.JwtHandshakeInterceptor;
import com.syncclipboard.ws.LegacyRejectingHandler;
import com.syncclipboard.ws.V2WebSocketHandler;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.socket.config.annotation.EnableWebSocket;
import org.springframework.web.socket.config.annotation.WebSocketConfigurer;
import org.springframework.web.socket.config.annotation.WebSocketHandlerRegistry;
import org.springframework.web.socket.server.standard.ServletServerContainerFactoryBean;

import java.util.List;

@Configuration
@EnableWebSocket
public class WebSocketConfig implements WebSocketConfigurer {

    private final V2WebSocketHandler v2Handler;
    private final JwtHandshakeInterceptor jwtInterceptor;

    @Value("${syncclipboard.origins.allowed-patterns:}")
    private List<String> allowedOriginPatterns;

    public WebSocketConfig(V2WebSocketHandler v2Handler,
                           JwtHandshakeInterceptor jwtInterceptor) {
        this.v2Handler = v2Handler;
        this.jwtInterceptor = jwtInterceptor;
    }

    @Override
    public void registerWebSocketHandlers(WebSocketHandlerRegistry registry) {
        // v2 endpoint: requires JWT bearer token via Sec-WebSocket-Protocol header.
        var v2 = registry.addHandler(v2Handler, "/ws/v2/clipboard")
                .addInterceptors(jwtInterceptor);
        if (allowedOriginPatterns == null || allowedOriginPatterns.isEmpty()) {
            v2.setAllowedOrigins("*");
        } else {
            v2.setAllowedOriginPatterns(allowedOriginPatterns.toArray(new String[0]));
        }

        // Legacy v1 path returns close 1003 with a clear "upgrade required" message.
        registry.addHandler(new LegacyRejectingHandler(), "/ws/clipboard")
                .setAllowedOrigins("*");
    }

    @Bean
    public ServletServerContainerFactoryBean createWebSocketContainer() {
        ServletServerContainerFactoryBean container = new ServletServerContainerFactoryBean();
        container.setMaxTextMessageBufferSize(2 * 1024 * 1024);
        container.setMaxBinaryMessageBufferSize(2 * 1024 * 1024);
        return container;
    }
}
