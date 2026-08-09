#!/bin/sh
set -eu
dir=$(dirname "$0")
exec "$dir/relisten" "$dir/../scheduler/icecc-scheduler"
