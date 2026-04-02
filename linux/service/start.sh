#!/bin/bash

# Fallback: ensure DISPLAY is set
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:0
fi

# Fallback: find XAUTHORITY if not set (for XWayland on GNOME)
if [ -z "$XAUTHORITY" ]; then
    XAUTH_FILE=$(find /run/user/$(id -u) -maxdepth 1 -name '.mutter-Xwaylandauth.*' 2>/dev/null | head -1)
    if [ -n "$XAUTH_FILE" ]; then
        export XAUTHORITY="$XAUTH_FILE"
    fi
fi

cd /home/joe/App/SyncClipboard
exec ./sync_clipboard config.ini
