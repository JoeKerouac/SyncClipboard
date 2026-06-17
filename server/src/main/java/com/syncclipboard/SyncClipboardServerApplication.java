package com.syncclipboard;

import com.syncclipboard.cli.UserAdminCommand;
import com.syncclipboard.config.AppProperties;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;

@SpringBootApplication
@EnableConfigurationProperties(AppProperties.class)
public class SyncClipboardServerApplication {
    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("user")) {
            // Forward relevant Spring-style args to system properties so CLI reads them.
            for (String arg : args) {
                if (arg.startsWith("--syncclipboard.users-file=")) {
                    System.setProperty("syncclipboard.users-file", arg.substring("--syncclipboard.users-file=".length()));
                }
            }
            // Strip "user" prefix and any --xxx=yyy args, pass remaining to CLI.
            java.util.List<String> sub = new java.util.ArrayList<>();
            for (int i = 1; i < args.length; i++) {
                if (!args[i].startsWith("--")) sub.add(args[i]);
            }
            UserAdminCommand.main(sub.toArray(new String[0]));
            return;
        }
        SpringApplication.run(SyncClipboardServerApplication.class, args);
    }
}
