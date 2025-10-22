#!/bin/sh

DAEMON=/usr/bin/aesdsocket
DAEMON_NAME=aesdsocket
DAEMON_OPTS="-d"   # run in daemon mode

case "$1" in
  start)
    echo "Starting $DAEMON_NAME"
    # -S = start if not running, -n = name match, -a = path to daemon
    start-stop-daemon -S -n "$DAEMON_NAME" -a "$DAEMON" -- $DAEMON_OPTS
    ;;
  stop)
    echo "Stopping $DAEMON_NAME"
    # -K sends SIGTERM by default; explicit shown for clarity
    start-stop-daemon -K -n "$DAEMON_NAME" --signal TERM
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
    ;;
esac

exit 0
