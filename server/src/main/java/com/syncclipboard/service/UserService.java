package com.syncclipboard.service;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.security.crypto.password.PasswordEncoder;
import org.springframework.stereotype.Service;

import jakarta.annotation.PostConstruct;
import java.io.*;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

@Service
public class UserService {

    private static final Logger log = LoggerFactory.getLogger(UserService.class);

    /**
     * Pre-computed BCrypt hash of an unguessable random string. Used to balance
     * authenticate() time when the username does not exist so callers cannot
     * tell apart "unknown user" and "wrong password" by timing.
     */
    private static final String DUMMY_HASH =
            "$2a$10$RIK.n.pSnznfFFu/n1pq1.ls..KAYKIUfa9FL13u6H1wEs5s2V7NC";

    private final PasswordEncoder encoder;

    @Value("${syncclipboard.users-file:data/users.properties}")
    private String usersFile;

    private final Map<String, String> users = new ConcurrentHashMap<>();

    public UserService(PasswordEncoder encoder) {
        this.encoder = encoder;
    }

    @PostConstruct
    public void init() {
        File file = new File(usersFile);
        if (!file.exists()) {
            log.error("Users file '{}' not found. Create it via the user-admin CLI:", file.getAbsolutePath());
            log.error("  java -cp <jar> com.syncclipboard.cli.UserAdminCommand add <username> <password>");
            return;
        }
        try (InputStream is = new FileInputStream(file)) {
            Properties props = new Properties();
            props.load(is);
            for (String key : props.stringPropertyNames()) {
                users.put(key, props.getProperty(key));
            }
        } catch (IOException e) {
            log.error("Failed to load users file: {}", e.getMessage());
            return;
        }

        boolean migrated = false;
        for (Map.Entry<String, String> entry : users.entrySet()) {
            String password = entry.getValue();
            if (!isBcrypt(password)) {
                log.warn("Plain-text password detected for user '{}'. Rehashing with BCrypt and overwriting users file.",
                        entry.getKey());
                users.put(entry.getKey(), encoder.encode(password));
                migrated = true;
            }
        }
        if (migrated) {
            saveUsers();
        }
        log.info("Loaded {} user(s) from {}", users.size(), file.getAbsolutePath());
    }

    /**
     * Authenticate a username/password pair. Always performs a BCrypt verify
     * (against a dummy hash for unknown users) so callers cannot use timing
     * to enumerate usernames.
     */
    public synchronized boolean authenticate(String username, String password) {
        if (username == null || password == null) {
            encoder.matches("dummy", DUMMY_HASH);
            return false;
        }
        String stored = users.get(username);
        if (stored == null) {
            encoder.matches(password, DUMMY_HASH);
            return false;
        }
        if (isBcrypt(stored)) {
            return encoder.matches(password, stored);
        }
        // Stored password is plaintext — compare directly, then auto-migrate.
        if (stored.equals(password)) {
            log.info("Auto-migrating plaintext password for user '{}' to BCrypt", username);
            users.put(username, encoder.encode(password));
            saveUsers();
            return true;
        }
        return false;
    }

    private static boolean isBcrypt(String s) {
        return s != null && (s.startsWith("$2a$") || s.startsWith("$2b$") || s.startsWith("$2y$"));
    }

    /** Used by the user-admin CLI to add or update a user. */
    public synchronized void upsertUser(String username, String password) {
        users.put(username, encoder.encode(password));
        saveUsers();
    }

    public synchronized boolean removeUser(String username) {
        boolean removed = users.remove(username) != null;
        if (removed) {
            saveUsers();
        }
        return removed;
    }

    public Set<String> listUsers() {
        return Set.copyOf(users.keySet());
    }

    private void saveUsers() {
        File file = new File(usersFile);
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            log.error("Failed to create users file directory: {}", parent.getAbsolutePath());
            return;
        }
        try (OutputStream os = new FileOutputStream(file)) {
            Properties props = new Properties();
            props.putAll(users);
            props.store(os, "SyncClipboard Users");
        } catch (IOException e) {
            log.error("Failed to save users file: {}", e.getMessage());
            return;
        }
        restrictPermissions(file.toPath());
    }

    private static void restrictPermissions(Path path) {
        try {
            if (Files.getFileAttributeView(path, java.nio.file.attribute.PosixFileAttributeView.class) != null) {
                Set<PosixFilePermission> perms = PosixFilePermissions.fromString("rw-------");
                Files.setPosixFilePermissions(path, perms);
            }
        } catch (IOException | UnsupportedOperationException ignored) {
            // best-effort; non-POSIX filesystems will skip
        }
    }
}
