#!/bin/bash
# Real-machine stress driver for the icecream deployment matrix.
#   matrix.sh <A|B|C|D|E|F>
# Topology: host A (STRESS_SCHED_IP) runs scheduler(s), the c-role
# stacks (unprivileged --no-remote iceccd + icecc clients), and one docker
# farm container; host B (STRESS_FARM_SSH) runs a native root iceccd farm
# daemon out of /dev/shm.  Versions per role are set by the config.
set -u
ST="$(cd "$(dirname "$0")" && pwd)"
MYIP=${STRESS_SCHED_IP:?set STRESS_SCHED_IP to this host LAN IP}
R6=${STRESS_FARM_SSH:?set STRESS_FARM_SSH to user@farmhost}
SOCKDIR=/run/user/$(id -u)/ice
ENVTAR=${STRESS_ENV_TAR:?set STRESS_ENV_TAR to an icecc-create-env tarball}
CFG=${1:?config A-F}
TS=$(date +%H%M%S)
RUN=$ST/logs/$CFG-$TS
mkdir -p $RUN $SOCKDIR

PO=8767   # s@o ports (text: 8768)
PX=8769   # s@x ports (text: 8770)

log() { echo "[$CFG] $*"; }

teardown() {
    pkill -f 'bundle/bin/./icecc-scheduler' 2>/dev/null
    pkill -f 'bundle/bin/./iceccd' 2>/dev/null
    docker rm -f farm-nas 2>/dev/null >/dev/null
    timeout 20 ssh $R6 "sudo -n pkill -f icestress/docker/bin 2>/dev/null; true" >/dev/null 2>&1
    rm -f $SOCKDIR/*.sock
    sleep 1
}

start_sched() { # ver port extra-env...
    local v=$1 port=$2; shift 2
    env "$@" $ST/bundle/bin/$v/icecc-scheduler -p $port -l $RUN/sched-$v.log -vvv &
    echo $! > $RUN/sched-$v.pid
}

start_cdaemon() { # ver sock sched-spec extra-env...
    local v=$1 sock=$2 sched=$3; shift 3
    mkdir -p $RUN/envcache-c$v
    env ICECC_TEST_SOCKET=$SOCKDIR/$sock "$@" $ST/bundle/bin/$v/iceccd --no-remote -m 2 \
        -s "$sched" -b $RUN/envcache-c$v -l $RUN/cdaemon-$v.log -N cnode-$v -vvv &
    echo $! > $RUN/cdaemon-$v.pid
}

start_farm_nas() { # ver sched-spec extra-docker-env...
    local v=$1 sched=$2; shift 2
    docker run -d --name farm-nas --net=host --cpus=12 "$@" icefarm:stress \
        $v farm-nas-$v 10245 "$sched" 14 >/dev/null
}

start_farm_r6() { # ver sched-spec extra-env-string
    local v=$1 sched=$2 extra=${3:-}
    timeout 25 ssh $R6 "sudo -n bash -c 'mkdir -p /dev/shm/icestress/envcache && rm -f /dev/shm/icestress/iceccd.log && $extra /dev/shm/icestress/docker/bin/$v/iceccd -d -N farm-r6-$v -p 10245 -s $sched -m 16 -u mickg -b /dev/shm/icestress/envcache -l /dev/shm/icestress/iceccd.log -vvv'"
}

run_build() { # label client-ver sock jobs
    local label=$1 v=$2 sock=$3 jobs=$4
    local W=$RUN/work-$label
    cp -r $ST/work $W
    log "build $label: client=$v -j$jobs"
    ( cd $W && /usr/bin/time -f "WALL %e" -o $RUN/time-$label.txt \
        make -k -j$jobs \
        CXX="env ICECC_TEST_SOCKET=$SOCKDIR/$sock ICECC_VERSION=$ENVTAR $ST/bundle/bin/$v/icecc g++" \
        > $RUN/build-$label.log 2>&1 )
    local rc=$?
    local objs=$(ls $W/src/*.o 2>/dev/null | wc -l)
    echo "$label rc=$rc objs=$objs/400 $(cat $RUN/time-$label.txt 2>/dev/null | tr '\n' ' ')" >> $RUN/SUMMARY
}

listcs() { # port label
    (echo listcs; sleep 1) | timeout 5 nc $MYIP $1 > $RUN/listcs-$2.txt 2>/dev/null
}

report() {
    echo "=== $CFG results ($RUN)"
    cat $RUN/SUMMARY 2>/dev/null
    for sl in $RUN/sched-*.log; do
        [ -f "$sl" ] || continue
        echo "--- $(basename $sl): job distribution:"
        grep -oE "put [0-9]+ in joblist of [a-zA-Z0-9_-]+" $sl | awk '{print $NF}' | sort | uniq -c
        echo "    errors: removed-daemons=$(grep -c 'remove daemon' $sl) failed-deliver=$(grep -c 'failed to deliver' $sl) deferrals=$(grep -c 'deferring' $sl)"
    done
    for bl in $RUN/build-*.log; do
        [ -f "$bl" ] || continue
        echo "--- $(basename $bl): got-UNKNOWN=$(grep -c 'got UNKNOWN' $bl) local-forced=$(grep -c 'local build forced' $bl) icecc-warnings=$(grep -cE 'ICECC.*(warning|error)' $bl)"
    done
}

trap 'teardown' EXIT
teardown

case $CFG in
A)  # c@o f@o s@x
    start_sched x $PX
    sleep 1
    start_farm_nas o $MYIP:$PX
    start_farm_r6 o $MYIP:$PX
    start_cdaemon o co.sock $MYIP:$PX
    sleep 4
    run_build main o co.sock 40
    listcs $((PX+1)) sx
    ;;
B)  # c@o f@x s@x
    start_sched x $PX
    sleep 1
    start_farm_nas x $MYIP:$PX
    start_farm_r6 x $MYIP:$PX
    start_cdaemon o co.sock $MYIP:$PX
    sleep 4
    run_build main o co.sock 40
    listcs $((PX+1)) sx
    ;;
C)  # all @x
    start_sched x $PX
    sleep 1
    start_farm_nas x $MYIP:$PX
    start_farm_r6 x $MYIP:$PX
    start_cdaemon x cx.sock $MYIP:$PX
    sleep 4
    run_build main x cx.sock 40
    listcs $((PX+1)) sx
    ;;
D)  # c@x f@o s@o
    start_sched o $PO
    sleep 1
    start_farm_nas o $MYIP:$PO
    start_farm_r6 o $MYIP:$PO
    start_cdaemon x cx.sock $MYIP:$PO
    sleep 4
    run_build main x cx.sock 40
    listcs $((PO+1)) so
    ;;
E)  # dual schedulers + discovery/election + LIVE failover mid-build
    EP="ICECC_TEST_SCHEDULER_PORTS=$PO:$PX"
    start_sched o $PO $EP
    sleep 1
    start_sched x $PX $EP
    sleep 2
    start_farm_nas x :$PO -e ICECC_TEST_SCHEDULER_PORTS=$PO:$PX
    start_farm_r6 x :$PO "ICECC_TEST_SCHEDULER_PORTS=$PO:$PX"
    env $EP true # (documentation of intent)
    ICECC_TEST_SCHEDULER_PORTS=$PO:$PX start_cdaemon x cx.sock :$PO $EP
    sleep 6
    log "who did daemons choose? (expect s@x, protocol 48)"
    grep -c "login" $RUN/sched-x.log; grep -c "login" $RUN/sched-o.log
    # build in background; kill s@x mid-build; restart it later
    run_build main x cx.sock 40 &
    BPID=$!
    sleep 25
    log "KILLING s@x mid-build (failover to s@o expected)"
    kill $(cat $RUN/sched-x.pid) 2>/dev/null
    sleep 30
    log "RESTARTING s@x (daemons should be evicted from s@o and rehome)"
    start_sched x $PX $EP
    wait $BPID
    listcs $((PX+1)) sx-after
    ;;
F)  # s@x f@x c@o+x concurrently
    start_sched x $PX
    sleep 1
    start_farm_nas x $MYIP:$PX
    start_farm_r6 x $MYIP:$PX
    start_cdaemon o co.sock $MYIP:$PX
    start_cdaemon x cx.sock $MYIP:$PX
    sleep 4
    run_build old o co.sock 20 &
    P1=$!
    run_build new x cx.sock 20 &
    P2=$!
    wait $P1 $P2
    listcs $((PX+1)) sx
    ;;
*) echo "unknown config $CFG"; exit 2;;
esac

report
