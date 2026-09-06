#!/bin/sh
set -eu

port=
netname=
assignment_fence_mode=
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
        --assignment-fence-mode)
            test "$#" -ge 2
            case "$2" in
                legacy|advisory|enforcing-compat|strict-nonce)
                    assignment_fence_mode=$2
                    ;;
                *)
                    echo "entry-scheduler: invalid assignment fence mode: $2" >&2
                    exit 64
                    ;;
            esac
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
runtime_user=nobody
set -- \
    -p "$port" \
    -n "$netname" \
    -u "$runtime_user" \
    -l /var/log/icecream/scheduler.log \
    -vvv
if test -n "$assignment_fence_mode"
then
    set -- "$@" --assignment-fence-mode "$assignment_fence_mode"
fi
exec /opt/icecream/sbin/icecc-scheduler "$@"
