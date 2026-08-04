#!/bin/bash
# icefarm container entrypoint: run one iceccd of the requested version.
#   entrypoint.sh <o|x> <node-name> <daemon-port> <scheduler-spec> <max-jobs> [extra args...]
# scheduler-spec is passed to -s (host:port); runs as root inside the
# container so chroot-based remote compilation works, jobs drop to 'nobody'.
set -e
V=${1:?version o|x}; NAME=${2:?name}; PORT=${3:?port}; SCHED=${4:?scheduler}; MAX=${5:?maxjobs}
shift 5
exec /ice/bin/$V/iceccd -l /dev/stdout -N "$NAME" -p "$PORT" -s "$SCHED" -m "$MAX" -u nobody -b /var/cache/icecream -vvv "$@" 2>&1
