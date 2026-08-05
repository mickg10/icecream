#!/bin/bash
# G-series: real-infrastructure backpressure tests.
#   gtest.sh G1  -> s@x shimmed, freeze c-daemon 12s  (transient: expect deferrals, zero loss)
#   gtest.sh G2  -> s@x shimmed, freeze c-daemon 45s  (bound: expect ~30s teardown, graceful recovery)
#   gtest.sh G3  -> s@o shimmed, freeze c-daemon 45s  (base: expect the original wedge + storm)
# The shim (SO_SNDBUF=4096 on accepted sockets, TCP_USER_TIMEOUT stripped)
# recreates the issue-report conditions; -j200 keeps ~200 dispatch replies
# in flight so the freeze actually jams the socket.
set -u
ST="$(cd "$(dirname "$0")" && pwd)"
MYIP=${STRESS_SCHED_IP:?set STRESS_SCHED_IP to this host LAN IP}
R6=${STRESS_FARM_SSH:?set STRESS_FARM_SSH to user@farmhost}
SOCKDIR=/run/user/$(id -u)/ice
ENVTAR=${STRESS_ENV_TAR:?set STRESS_ENV_TAR to an icecc-create-env tarball}
SHIM=$ST/sndbuf_shim.so
CFG=${1:?G1|G2|G3}
TS=$(date +%H%M%S)
RUN=$ST/logs/$CFG-$TS
mkdir -p $RUN $SOCKDIR

case $CFG in
  G1) SV=x; FREEZE=12;;
  G2) SV=x; FREEZE=45;;
  G3) SV=o; FREEZE=45;;
  *) echo bad config; exit 2;;
esac
PORT=8769
CV=$SV   # match client/farm version to scheduler generation (C vs all-o topology)

log() { echo "[$CFG] $*"; }
teardown() {
    pkill -f 'bundle/bin/./icecc-schedule[r]' 2>/dev/null
    pkill -f 'bundle/bin/./icecc[d]' 2>/dev/null
    docker rm -f farm-nas 2>/dev/null >/dev/null
    timeout 20 ssh $R6 "sudo -n pkill -f 'icestress/docker/bi[n]' 2>/dev/null; true" >/dev/null 2>&1
    rm -f $SOCKDIR/*.sock; sleep 1
}
trap teardown EXIT
teardown

# shimmed scheduler
env LD_PRELOAD=$SHIM ICECC_TEST_SNDBUF=4096 ICECC_TEST_STRIP_USER_TIMEOUT=1 \
    $ST/bundle/bin/$SV/icecc-scheduler -p $PORT -l $RUN/sched.log -vvv &
echo $! > $RUN/sched.pid
sleep 1
docker run -d --name farm-nas --net=host --cpus=12 icefarm:stress $CV farm-nas-$CV 10245 $MYIP:$PORT 14 >/dev/null
timeout 25 ssh $R6 "sudo -n bash -c 'mkdir -p /dev/shm/icestress/envcache && /dev/shm/icestress/docker/bin/$CV/iceccd -d -N farm-r6-$CV -p 10245 -s $MYIP:$PORT -m 16 -u mickg -b /dev/shm/icestress/envcache -l /dev/shm/icestress/iceccd.log -vvv'"
mkdir -p $RUN/envcache-c
env LD_PRELOAD=$SHIM ICECC_TEST_RCVBUF=4096 \
    ICECC_TEST_SOCKET=$SOCKDIR/c.sock $ST/bundle/bin/$CV/iceccd --no-remote -m 2 \
    -s $MYIP:$PORT -b $RUN/envcache-c -l $RUN/cdaemon.log -N cnode-$CV -vvv &
CDPID=$!
sleep 4

# responsiveness probe on the text port, 1 round/s, logs latency
python3 - "$MYIP" $((PORT+1)) $RUN/probe.txt <<'PYEOF' &
import socket, sys, time
host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = open(out, 'w', buffering=1)
s = socket.create_connection((host, port), timeout=10)
s.sendall(b"\n")  # complete login prompt round
time.sleep(0.5); s.recv(4096)
while True:
    t0 = time.time()
    try:
        s.sendall(b"listcs\n")
        s.settimeout(120)
        s.recv(8192)
        f.write("%.1f %.2f\n" % (time.time() % 1000, time.time() - t0))
    except Exception as e:
        f.write("DEAD %s\n" % e); break
    time.sleep(1)
PYEOF
PROBEPID=$!

W=$RUN/work; cp -r $ST/work $W
log "build starts (-j400); will freeze c-daemon for ${FREEZE}s once 300 requests are queued"
( cd $W && /usr/bin/time -f "WALL %e" -o $RUN/time.txt make -k -j400 \
    CXX="env ICECC_TEST_SOCKET=$SOCKDIR/c.sock ICECC_VERSION=$ENVTAR $ST/bundle/bin/$CV/icecc g++" \
    > $RUN/build.log 2>&1 ) &
BPID=$!
# Deterministic freeze point: wait until >=300 job requests have reached the
# scheduler (farm has only ~30 slots, so ~270+ are queued and their dispatch
# replies will trickle into the frozen daemon's socket at slot-free rate).
for i in $(seq 1 300); do
    # NB: grep -c prints 0 itself when nothing matches; an || echo 0 here
    # would emit a second line and break the -ge comparison.
    n=$(grep -c "NEW " $RUN/sched.log 2>/dev/null)
    n=${n:-0}
    [ "$n" -ge 300 ] && break
    sleep 0.1
done
log "freezing at $n forwarded requests"
log "SIGSTOP c-daemon (all its processes)"
pkill -STOP -f 'bundle/bin/./icecc[d]'
sleep $FREEZE
log "SIGCONT c-daemon"
pkill -CONT -f 'bundle/bin/./icecc[d]'
wait $BPID
BUILD_RC=$?
kill $PROBEPID 2>/dev/null

OBJS=$(ls $W/src/*.o 2>/dev/null | wc -l)
echo "=== $CFG results ($RUN)"
echo "objs=$OBJS/400 $(cat $RUN/time.txt | tr '\n' ' ')"
echo "sched: deferrals=$(grep -c 'deferring' $RUN/sched.log) bound-teardowns=$(grep -c 'has not accepted dispatch data' $RUN/sched.log) removed=$(grep -c 'remove daemon' $RUN/sched.log) timed-out-send=$(grep -c 'timed out while trying to send' $RUN/sched.log)"
echo "build: got-UNKNOWN=$(grep -c 'got UNKNOWN' $RUN/build.log) local-forced=$(grep -c 'local build forced' $RUN/build.log) failed-TUs=$(grep -cE '^make.*Error' $RUN/build.log)"
echo "probe: worst-latency=$(awk '{if($2>m)m=$2} END{print m"s"}' $RUN/probe.txt 2>/dev/null) rounds=$(wc -l < $RUN/probe.txt)"
grep -oE "put [0-9]+ in joblist of [a-zA-Z0-9_-]+" $RUN/sched.log | awk '{print $NF}' | sort | uniq -c
# Enforced expectations per scenario: G1 must lose nothing; G2/G3 tolerate
# in-flight casualties of the long freeze but the build run must complete.
FAIL=0
DEFERRALS=$(grep -c 'deferring' $RUN/sched.log 2>/dev/null); DEFERRALS=${DEFERRALS:-0}
TEARDOWNS=$(grep -c 'has not accepted dispatch data' $RUN/sched.log 2>/dev/null); TEARDOWNS=${TEARDOWNS:-0}
WORST=$(awk '{if($2>m)m=$2} END{printf "%.0f", m}' $RUN/probe.txt 2>/dev/null); WORST=${WORST:-0}
case $CFG in
  G1) # transient freeze: nothing lost, scheduler responsive
      { [ "$BUILD_RC" -eq 0 ] && [ "$OBJS" -eq 400 ] && [ "$WORST" -lt 5 ]; } || FAIL=1;;
  G2) # long freeze on the fixed scheduler: build completes, stays responsive
      { [ "$BUILD_RC" -eq 0 ] && [ "$WORST" -lt 5 ]; } || FAIL=1;;
  G3) # baseline: only completion is required (it is expected to wedge)
      [ "$BUILD_RC" -eq 0 ] || FAIL=1;;
esac
echo "asserted: rc=$BUILD_RC objs=$OBJS deferrals=$DEFERRALS teardowns=$TEARDOWNS worst_probe=${WORST}s"
if [ "$FAIL" -ne 0 ]; then echo "RESULT: FAIL"; exit 1; fi
echo "RESULT: PASS"
