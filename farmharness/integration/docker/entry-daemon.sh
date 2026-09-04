#!/bin/sh
set -eu

scheduler=
netname=
port=
slots=
node_name=
while test "$#" -gt 0
do
    case "$1" in
        --scheduler)
            test "$#" -ge 2
            scheduler=$2
            shift 2
            ;;
        --slots)
            test "$#" -ge 2
            slots=$2
            shift 2
            ;;
        --netname)
            test "$#" -ge 2
            netname=$2
            shift 2
            ;;
        --port)
            test "$#" -ge 2
            port=$2
            shift 2
            ;;
        --name)
            test "$#" -ge 2
            node_name=$2
            shift 2
            ;;
        *)
            echo "entry-daemon: unsupported argument: $1" >&2
            exit 64
            ;;
    esac
done
test -n "$scheduler"
test -n "$netname"
test -n "$port"
test -n "$slots"
test -n "$node_name"

install -d -m 1777 /var/cache/icecream/envs /var/log/icecream
install -d -m 0700 -o icecc -g icecc /var/cache/icecream/p50-runtime
chown -R icecc:icecc /var/cache/icecream

set -- \
    -s "$scheduler" \
    -n "$netname" \
    -p "$port" \
    -m "$slots" \
    -N "$node_name" \
    -u icecc \
    -b /var/cache/icecream/envs \
    -l /var/log/icecream/iceccd.log \
    -vvv
if test -x /opt/icecream/sbin/icecc-cache-service
then
    set -- "$@" \
        --cache-service /opt/icecream/sbin/icecc-cache-service \
        --cache-runtime-dir /var/cache/icecream/p50-runtime
fi
exec /opt/icecream/sbin/iceccd "$@"
