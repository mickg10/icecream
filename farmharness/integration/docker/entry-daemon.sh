#!/bin/sh
set -eu

scheduler=
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
test -n "$slots"
test -n "$node_name"

install -d -m 1777 /var/cache/icecream/envs /var/log/icecream
install -d -m 0700 -o icecc -g icecc /var/cache/icecream/p50-runtime
chown -R icecc:icecc /var/cache/icecream

set -- \
    -s "$scheduler" \
    -m "$slots" \
    -N "$node_name" \
    -u "$(id -u icecc)" \
    -b /var/cache/icecream/envs \
    -l /var/log/icecream/iceccd.log \
    -vv
if test -x /opt/icecream/sbin/icecc-cache-service
then
    set -- "$@" \
        --cache-service /opt/icecream/sbin/icecc-cache-service \
        --cache-runtime-dir /var/cache/icecream/p50-runtime
fi
exec /opt/icecream/sbin/iceccd "$@"
