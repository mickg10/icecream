#!/bin/sh
# Connectivity regression under the failure-injection preload.
dir=$(dirname "$0")
LD_PRELOAD="$dir/conn_shim.so" "$dir/connectivity"
