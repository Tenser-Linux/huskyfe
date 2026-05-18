case "$(tty)" in
    /dev/tty[0-9]*)
        if ! pgrep -x huskyfe >/dev/null 2>&1; then
            pkill -9 -f 'wcomp|cage|phoc|phosh|weston|modetest' 2>/dev/null
            sleep 1

            if ! pgrep -f glproxy-srv >/dev/null 2>&1; then
                chroot /var/lib/machines/halium /usr/local/bin/glproxy-srv \
                    > /tmp/glp.log 2>&1 &
                sleep 1
            fi

            exec /root/huskyfe/huskyfe > /tmp/huskyfe.log 2>&1
        fi
        ;;
esac
