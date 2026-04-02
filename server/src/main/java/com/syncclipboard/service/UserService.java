package com.syncclipboard.service;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.security.crypto.bcrypt.BCryptPasswordEncoder;
import org.springframework.stereotype.Service;

import jakarta.annotation.PostConstruct;
import java.io.*;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.ConcurrentHashMap;

@Service
public class UserService {

    private static final Logger log = LoggerFactory.getLogger(UserService.class);
    private final BCryptPasswordEncoder encoder = new BCryptPasswordEncoder();

    @Value("${syncclipboard.users-file:users.properties}")
    private String usersFile;

    private final Map<String, String> users = new ConcurrentHashMap<>();

    @PostConstruct
    public void init() {
        File file = new File(usersFile);
        if (file.exists()) {
            try (InputStream is = new FileInputStream(file)) {
                Properties props = new Properties();
                props.load(is);
                for (String key : props.stringPropertyNames()) {
                    users.put(key, props.getProperty(key));
                }
            } catch (IOException e) {
                log.error("加载用户文件失败: {}", e.getMessage());
            }
        }
        if (users.isEmpty()) {
            users.put("admin", encoder.encode("admin123"));
            saveUsers();
            log.info("已创建默认用户: admin / admin123");
        } else {
            boolean migrated = false;
            for (Map.Entry<String, String> entry : users.entrySet()) {
                String password = entry.getValue();
                if (!password.startsWith("$2a$") && !password.startsWith("$2b$")) {
                    users.put(entry.getKey(), encoder.encode(password));
                    migrated = true;
                }
            }
            if (migrated) {
                saveUsers();
                log.info("已将明文密码迁移为BCrypt哈希");
            }
        }
        log.info("已加载 {} 个用户", users.size());
    }

    public boolean authenticate(String username, String password) {
        String stored = users.get(username);
        return stored != null && encoder.matches(password, stored);
    }

    private void saveUsers() {
        try (OutputStream os = new FileOutputStream(usersFile)) {
            Properties props = new Properties();
            props.putAll(users);
            props.store(os, "SyncClipboard Users");
        } catch (IOException e) {
            log.error("保存用户文件失败: {}", e.getMessage());
        }
    }
}
