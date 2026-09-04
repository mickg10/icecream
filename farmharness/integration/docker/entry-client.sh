#!/bin/sh
set -eu

scheduler=
idle=false
while test "$#" -gt 0
do
    case "$1" in
        --scheduler)
            test "$#" -ge 2
            scheduler=$2
            shift 2
            ;;
        --idle)
            idle=true
            shift
            ;;
        *)
            echo "entry-client: unsupported argument: $1" >&2
            exit 64
            ;;
    esac
done
test -n "$scheduler"
test "$idle" = true

install -d -m 1777 /var/cache/icecream/envs /var/log/icecream
install -d -m 0700 -o icecc -g icecc /var/cache/icecream/p50-runtime
chown -R icecc:icecc /var/cache/icecream

set -- \
    -s "$scheduler" \
    -m 0 \
    --no-remote \
    -N client \
    -u "$(id -u icecc)" \
    -b /var/cache/icecream/envs \
    -l /var/log/icecream/client-daemon.log \
    -vv
if test -x /opt/icecream/sbin/icecc-cache-service
then
    set -- "$@" \
        --cache-service /opt/icecream/sbin/icecc-cache-service \
        --cache-runtime-dir /var/cache/icecream/p50-runtime
fi
export ICECC_CARET_WORKAROUND=0
exec /opt/icecream/sbin/iceccd "$@"
