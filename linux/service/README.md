注意，服务需要安装在用户级systemd下（`~/.config/systemd/user/`），启动的时候如果报 `Failed to connect to bus: No medium found` 这个错，那么可以使用下面的方式启动：

```shell
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/systemctl --user start sync-clipboard.service
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/systemctl --user stop sync-clipboard.service
```

