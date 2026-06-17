package com.syncclipboard.cli;

import org.springframework.security.crypto.bcrypt.BCryptPasswordEncoder;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;

/**
 * Command line tool to manage the users.properties file without requiring the
 * Spring Boot context. Run with:
 *   java -cp sync-clipboard-server-1.0.0.jar com.syncclipboard.cli.UserAdminCommand <cmd> [args]
 *
 * Commands:
 *   add <username> <password>      Add or replace a user (BCrypt hashed)
 *   passwd <username> <password>   Alias of add
 *   remove <username>              Remove a user
 *   list                           List usernames
 */
public final class UserAdminCommand {

    private UserAdminCommand() {}

    public static void main(String[] args) throws IOException {
        if (args.length == 0) {
            usage(1);
        }
        String usersFile = System.getProperty("syncclipboard.users-file", "data/users.properties");
        File file = new File(usersFile);
        Properties props = load(file);

        String cmd = args[0];
        switch (cmd) {
            case "add", "passwd" -> {
                if (args.length != 3) usage(1);
                String username = args[1];
                String password = args[2];
                BCryptPasswordEncoder enc = new BCryptPasswordEncoder();
                props.setProperty(username, enc.encode(password));
                save(file, props);
                System.out.println("OK: user '" + username + "' upserted in " + file.getAbsolutePath());
            }
            case "remove" -> {
                if (args.length != 2) usage(1);
                String username = args[1];
                if (props.remove(username) != null) {
                    save(file, props);
                    System.out.println("OK: user '" + username + "' removed.");
                } else {
                    System.err.println("WARN: user '" + username + "' not found.");
                    System.exit(2);
                }
            }
            case "list" -> {
                Set<String> sorted = new TreeSet<>(props.stringPropertyNames());
                if (sorted.isEmpty()) {
                    System.out.println("(no users)");
                } else {
                    sorted.forEach(System.out::println);
                }
            }
            default -> usage(1);
        }
    }

    private static Properties load(File file) throws IOException {
        Properties props = new Properties();
        if (file.exists()) {
            try (InputStream is = new FileInputStream(file)) {
                props.load(is);
            }
        }
        return props;
    }

    private static void save(File file, Properties props) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Cannot create directory: " + parent.getAbsolutePath());
        }
        try (OutputStream os = new FileOutputStream(file)) {
            props.store(os, "SyncClipboard Users (managed by UserAdminCommand)");
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
            // best-effort
        }
    }

    private static void usage(int exitCode) {
        System.err.println("Usage:");
        System.err.println("  UserAdminCommand add <username> <password>");
        System.err.println("  UserAdminCommand passwd <username> <password>");
        System.err.println("  UserAdminCommand remove <username>");
        System.err.println("  UserAdminCommand list");
        System.err.println();
        System.err.println("Override file path with -Dsyncclipboard.users-file=<path>");
        System.exit(exitCode);
    }
}
