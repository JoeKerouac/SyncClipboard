package com.syncclipboard.service;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.nio.charset.StandardCharsets;

/**
 * UDP reflector for NAT endpoint discovery.
 * Client sends any UDP packet; server responds with the client's public IP:port.
 */
@Service
public class UdpReflectorService {

    private static final Logger log = LoggerFactory.getLogger(UdpReflectorService.class);

    @Value("${syncclipboard.udp-port:8081}")
    private int udpPort;

    private volatile boolean running = true;
    private DatagramSocket socket;

    @PostConstruct
    public void start() {
        Thread t = new Thread(() -> {
            try {
                socket = new DatagramSocket(udpPort);
                log.info("[UDP] 反射器已启动 port={}", udpPort);
                byte[] buf = new byte[256];
                while (running) {
                    DatagramPacket packet = new DatagramPacket(buf, buf.length);
                    socket.receive(packet);
                    String publicEndpoint = packet.getAddress().getHostAddress() + ":" + packet.getPort();
                    byte[] resp = publicEndpoint.getBytes(StandardCharsets.UTF_8);
                    socket.send(new DatagramPacket(resp, resp.length, packet.getAddress(), packet.getPort()));
                    log.debug("[UDP] 反射 {} -> {}", publicEndpoint, publicEndpoint);
                }
            } catch (Exception e) {
                if (running) log.error("[UDP] 反射器异常", e);
            }
        }, "udp-reflector");
        t.setDaemon(true);
        t.start();
    }

    @PreDestroy
    public void stop() {
        running = false;
        if (socket != null) socket.close();
    }
}
