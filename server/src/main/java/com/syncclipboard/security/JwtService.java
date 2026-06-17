package com.syncclipboard.security;

import io.jsonwebtoken.Claims;
import io.jsonwebtoken.JwtException;
import io.jsonwebtoken.Jwts;
import io.jsonwebtoken.security.Keys;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import jakarta.annotation.PostConstruct;
import javax.crypto.SecretKey;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.SecureRandom;
import java.time.Duration;
import java.time.Instant;
import java.util.Base64;
import java.util.Date;
import java.util.Set;

/**
 * JWT signing and verification. The signing secret is persisted to
 * {@code data/jwt.key} on first start so existing tokens survive restarts.
 * Rotating the file invalidates every outstanding token.
 */
@Service
public class JwtService {

    private static final Logger log = LoggerFactory.getLogger(JwtService.class);
    private static final String CLAIM_DEVICE = "did";
    private static final String CLAIM_KIND = "knd";
    private static final String KIND_ACCESS = "access";
    private static final String KIND_REFRESH = "refresh";

    @Value("${syncclipboard.data-dir:data}")
    private String dataDir;

    @Value("${syncclipboard.auth.access-ttl:PT24H}")
    private Duration accessTtl;

    @Value("${syncclipboard.auth.refresh-ttl:P30D}")
    private Duration refreshTtl;

    private SecretKey signingKey;

    @PostConstruct
    public void init() throws IOException {
        Path keyFile = Path.of(dataDir, "jwt.key");
        if (Files.exists(keyFile)) {
            byte[] bytes = Files.readAllBytes(keyFile);
            byte[] decoded = Base64.getDecoder().decode(bytes);
            this.signingKey = Keys.hmacShaKeyFor(decoded);
            log.info("Loaded JWT signing key from {}", keyFile.toAbsolutePath());
        } else {
            byte[] bytes = new byte[64];
            new SecureRandom().nextBytes(bytes);
            this.signingKey = Keys.hmacShaKeyFor(bytes);
            Files.createDirectories(keyFile.getParent());
            Files.write(keyFile, Base64.getEncoder().encode(bytes));
            try {
                Files.setPosixFilePermissions(keyFile, PosixFilePermissions.fromString("rw-------"));
            } catch (UnsupportedOperationException ignored) {
                // non-POSIX filesystem
            }
            log.warn("Generated new JWT signing key at {}. Existing tokens (if any) are now invalid.",
                    keyFile.toAbsolutePath());
        }
    }

    public String issueAccessToken(String username, String deviceId) {
        return issue(username, deviceId, KIND_ACCESS, accessTtl);
    }

    public String issueRefreshToken(String username, String deviceId) {
        return issue(username, deviceId, KIND_REFRESH, refreshTtl);
    }

    public Duration accessTtl() {
        return accessTtl;
    }

    private String issue(String username, String deviceId, String kind, Duration ttl) {
        Instant now = Instant.now();
        return Jwts.builder()
                .subject(username)
                .claim(CLAIM_DEVICE, deviceId)
                .claim(CLAIM_KIND, kind)
                .issuedAt(Date.from(now))
                .expiration(Date.from(now.plus(ttl)))
                .signWith(signingKey, Jwts.SIG.HS256)
                .compact();
    }

    public ParsedToken verifyAccessToken(String token) {
        return verify(token, KIND_ACCESS);
    }

    public ParsedToken verifyRefreshToken(String token) {
        return verify(token, KIND_REFRESH);
    }

    private ParsedToken verify(String token, String expectedKind) {
        try {
            Claims claims = Jwts.parser()
                    .verifyWith(signingKey)
                    .build()
                    .parseSignedClaims(token)
                    .getPayload();
            if (!Set.of(KIND_ACCESS, KIND_REFRESH).contains(claims.get(CLAIM_KIND, String.class))) {
                return null;
            }
            String kind = claims.get(CLAIM_KIND, String.class);
            if (!expectedKind.equals(kind)) return null;
            return new ParsedToken(
                    claims.getSubject(),
                    claims.get(CLAIM_DEVICE, String.class),
                    claims.getExpiration().toInstant());
        } catch (JwtException | IllegalArgumentException e) {
            return null;
        }
    }

    public record ParsedToken(String username, String deviceId, Instant expiresAt) {}
}
