#!/bin/sh
set -eu

port=
netname=
while test "$#" -gt 0
do
    case "$1" in
        --port)
            test "$#" -ge 2
            port=$2
            shift 2
            ;;
        --netname)
            test "$#" -ge 2
            netname=$2
            shift 2
            ;;
        *)
            echo "entry-scheduler: unsupported argument: $1" >&2
            exit 64
            ;;
    esac
done
test -n "$port"
test -n "$netname"
exec /opt/icecream/sbin/icecc-scheduler \
    -p "$port" \
    -n "$netname" \
    -u icecc \
    -l /var/log/icecream/scheduler.log \
    -vvv
