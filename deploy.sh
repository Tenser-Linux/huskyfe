#!/usr/bin/env bash
# huskyfe deploy script — push, build, run on phone.
#
# Usage:
#   ./deploy.sh            push + build + run
#   ./deploy.sh push       push only
#   ./deploy.sh build      push + build only
#   ./deploy.sh run        run only (assumes already built)
#   ./deploy.sh clean      wipe build artifacts on phone
#
# Env overrides:
#   PHONE=root@192.168.x.y    target host (default: root@pixel)
#   REMOTE=/root/huskyfe       remote path (default: /root/huskyfe)

set -euo pipefail

PHONE="${PHONE:-root@192.168.1.148}"
REMOTE="${REMOTE:-/root/huskyfe}"
HERE="$(cd "$(dirname "$0")" && pwd)"

SSH_OPTS="-o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"

ssh_run()  { ssh $SSH_OPTS "$PHONE" "$@"; }
scp_send() { scp $SSH_OPTS -r "$@" "$PHONE:$REMOTE/"; }

cmd_push() {
    echo ">> pushing src/ Makefile deploy.sh to $PHONE:$REMOTE/"
    ssh_run "mkdir -p '$REMOTE'"
    scp_send "$HERE/src" "$HERE/Makefile" "$HERE/deploy.sh"
}

cmd_build() {
    echo ">> building on phone"
    ssh_run "cd '$REMOTE' && make -j4"
}

cmd_run() {
    echo ">> stopping huskyfe.service + display owners"
    ssh_run "systemctl stop huskyfe.service 2>/dev/null; pkill -9 -f 'wcomp|cage|phoc|phosh|weston|modetest|huskyfe' 2>/dev/null; sleep 1; true"

    echo ">> ensuring glproxy-srv.service is up"
    ssh_run "systemctl is-active glproxy-srv >/dev/null || systemctl start glproxy-srv; systemctl --no-pager is-active glproxy-srv"

    echo ">> launching huskyfe in background"
    ssh_run "cd '$REMOTE' && nohup ./huskyfe >/tmp/huskyfe.log 2>&1 </dev/null & sleep 1; pgrep -af '/huskyfe|^./huskyfe' | tail -n 1"
}

cmd_clean() {
    echo ">> cleaning $REMOTE"
    ssh_run "cd '$REMOTE' && make clean 2>/dev/null; true"
}

cmd_logs() {
    echo ">> tailing huskyfe.log + glproxy-srv journal"
    ssh $SSH_OPTS -t "$PHONE" "( tail -f /tmp/huskyfe.log 2>/dev/null & ) && journalctl -u glproxy-srv -f --no-pager"
}

cmd_install_profile() {
    echo ">> installing /root/.profile (auto-launch on local TTY)"
    scp $SSH_OPTS "$HERE/profile" "$PHONE:/root/.profile"
    ssh_run "chmod 644 /root/.profile && head -1 /root/.profile"
}

cmd_install_service() {
    echo ">> installing huskyfe.service to /etc/systemd/system/"
    scp $SSH_OPTS "$HERE/huskyfe.service" "$PHONE:/etc/systemd/system/huskyfe.service"
    ssh_run "systemctl daemon-reload && systemctl enable huskyfe.service && systemctl restart huskyfe.service && sleep 2 && systemctl status huskyfe.service --no-pager -l"
}

case "${1:-all}" in
    push)    cmd_push ;;
    build)   cmd_push; cmd_build ;;
    run)     cmd_run ;;
    clean)   cmd_clean ;;
    logs)    cmd_logs ;;
    profile) cmd_install_profile ;;
    service) cmd_install_service ;;
    all|"")  cmd_push; cmd_build; cmd_run ;;
    *)
        echo "usage: $0 [push|build|run|clean|logs|profile|service|all]" >&2
        exit 2
        ;;
esac
