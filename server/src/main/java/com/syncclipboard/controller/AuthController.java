package com.syncclipboard.controller;

import com.syncclipboard.security.JwtService;
import com.syncclipboard.security.LoginRateLimiter;
import com.syncclipboard.service.UserService;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import java.time.Instant;
import java.util.Map;

@RestController
@RequestMapping("/api/v2/auth")
public class AuthController {

    private static final Logger log = LoggerFactory.getLogger(AuthController.class);

    private final UserService userService;
    private final JwtService jwtService;
    private final LoginRateLimiter rateLimiter;

    public AuthController(UserService userService, JwtService jwtService, LoginRateLimiter rateLimiter) {
        this.userService = userService;
        this.jwtService = jwtService;
        this.rateLimiter = rateLimiter;
    }

    public record LoginRequest(@NotBlank String username, @NotBlank String password, @NotBlank String deviceId) {}

    public record RefreshRequest(@NotBlank String refreshToken, @NotBlank String deviceId) {}

    public record TokenPair(String accessToken, String refreshToken, long expiresInSec) {}

    @PostMapping("/login")
    public ResponseEntity<?> login(@Valid @RequestBody LoginRequest req, HttpServletRequest http) {
        String ip = http.getRemoteAddr();
        if (!rateLimiter.tryAcquire(req.username(), ip)) {
            log.warn("[AUTH] login rate limited user={}, ip={}", req.username(), ip);
            return ResponseEntity.status(429).body(Map.of(
                    "code", "RATE_LIMITED",
                    "message", "Too many failed attempts, please try again later"));
        }
        if (!userService.authenticate(req.username(), req.password())) {
            rateLimiter.recordFailure(req.username(), ip);
            log.warn("[AUTH] login failed user={}, ip={}", req.username(), ip);
            return ResponseEntity.status(401).body(Map.of(
                    "code", "AUTH_INVALID",
                    "message", "Invalid username or password"));
        }
        rateLimiter.recordSuccess(req.username(), ip);
        TokenPair tokens = issueTokens(req.username(), req.deviceId());
        log.info("[AUTH] login success user={}, device={}, ip={}", req.username(), req.deviceId(), ip);
        return ResponseEntity.ok(tokens);
    }

    @PostMapping("/refresh")
    public ResponseEntity<?> refresh(@Valid @RequestBody RefreshRequest req) {
        JwtService.ParsedToken parsed = jwtService.verifyRefreshToken(req.refreshToken());
        if (parsed == null || !parsed.deviceId().equals(req.deviceId())) {
            return ResponseEntity.status(401).body(Map.of(
                    "code", "AUTH_INVALID",
                    "message", "Refresh token invalid or expired"));
        }
        if (!userService.listUsers().contains(parsed.username())) {
            return ResponseEntity.status(401).body(Map.of(
                    "code", "AUTH_INVALID",
                    "message", "User no longer exists"));
        }
        TokenPair tokens = issueTokens(parsed.username(), parsed.deviceId());
        return ResponseEntity.ok(tokens);
    }

    private TokenPair issueTokens(String username, String deviceId) {
        String access = jwtService.issueAccessToken(username, deviceId);
        String refresh = jwtService.issueRefreshToken(username, deviceId);
        long ttlSec = jwtService.accessTtl().getSeconds();
        return new TokenPair(access, refresh, ttlSec);
    }
}
