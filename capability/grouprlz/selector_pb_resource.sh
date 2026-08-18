#!/usr/bin/env bash
# Policy-B RESOURCE pass: fixed-total-core-budget TU112 probe race, hardened.
#
# This is the OFFLINE CENSUS runner. It is NOT production policy B: production runs a
# cheap classifier and then only the selected codec, whereas this races both bounded
# probes to obtain the census. Numbers here bound the census cost, not the product.
#
# Addresses the four gating conditions:
#   1. the P29 raw-plane plan pass is INSIDE the candidate clock (P29 is two-pass today;
#      both passes are charged). A one-pass P29 would replace both.
#   2. refuses to start until the box is quiet (no competing sweep, load below a floor).
#   3. each candidate is charged its OWN input conversion: GRZ pays concat + TU map,
#      P29 pays its own file open + intern inside its process. Nothing is inferred by
#      subtracting stages. Labelled research-process: P29 still uses the research
#      interner, and the producer is not yet a shared one-producer fanout.
#   4. hardened: strict mode, both PIDs waited on and both statuses required, every
#      parsed field validated, bounded GRZ wire decoded and compared, concurrent P29
#      wire compared against the gated identity wire, immutable run root, full
#      resource + provenance record, every repetition reported.
set -euo pipefail

P=${1:?project}; PR=${2:?profile}
REPS=${REPS:-3}
P29_CORES=${P29_CORES:-16-23}
GRZ_CORES=${GRZ_CORES:-24-31}
LOAD_FLOOR=${LOAD_FLOOR:-1.5}
WAIT_MAX_S=${WAIT_MAX_S:-3600}
RUN_ROOT=${RUN_ROOT:?set RUN_ROOT to a NEW retained directory}

CELL=$HOME/ictmp/ii-matrix/$P/$PR
IDDIR=$HOME/selbind/pb/$P-$PR/id
P29=${P29:-$HOME/selbind/p29build/codec50-56c1744}
P29SRC=${P29SRC:-$HOME/selbind/p29build/codec50.cpp}
GRZ=${GRZ:-$HOME/issue16-selector-v1/tools/grz2g-selector}
GRZSRC=${GRZSRC:-$HOME/issue16-selector-v1/tools/grz2g-selector.cpp}

[[ ! -e $RUN_ROOT ]] || { echo "run root exists, refusing to overwrite: $RUN_ROOT" >&2; exit 1; }
[[ -x $P29 && -x $GRZ ]] || { echo "missing codec binary" >&2; exit 1; }
[[ -f $CELL/corpus.json && -f $CELL/manifest.tsv ]] || { echo "incomplete cell: $CELL" >&2; exit 1; }
[[ -f $IDDIR/prefix/literal.wire ]] || { echo "no gated identity wire: $IDDIR" >&2; exit 1; }
mkdir -p "$RUN_ROOT"/{input,p29,grz}

num() {  # validate a parsed field is a positive number, else abort loudly
    local v=$1 what=$2
    [[ $v =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "unparsable $what: '$v'" >&2; exit 3; }
    printf '%s' "$v"
}

# ---- condition 2: refuse to run on a busy box -------------------------------------
waited=0
while :; do
    # Exclude this script and its parent so the gate cannot match itself. Built without
    # a pipeline: under `set -e -o pipefail` an empty pgrep result is a non-zero status
    # and would abort the run rather than report a quiet box.
    mapfile -t _procs < <(pgrep -f 'run_selector_matrix|run_selector_cell|pbsweep\.sh|dmsweep\.sh|pbcell\.sh|dmcell\.sh|grz2g|codec50-' 2>/dev/null || true)
    busy=0
    for _p in ${_procs[@]+"${_procs[@]}"}; do
        [[ -n $_p && $_p != "$$" && $_p != "$PPID" ]] && busy=$((busy + 1))
    done
    load=$(cut -d' ' -f1 /proc/loadavg)
    if [[ ${busy:-0} -eq 0 ]] && awk -v l="$load" -v f="$LOAD_FLOOR" 'BEGIN{exit !(l<f)}'; then break; fi
    (( waited >= WAIT_MAX_S )) && { echo "box still busy after ${WAIT_MAX_S}s (load=$load busy=$busy)" >&2; exit 4; }
    sleep 30; waited=$((waited+30))
done
printf 'quiesced after %ss, load=%s\n' "$waited" "$(cut -d' ' -f1 /proc/loadavg)"

# ---- inputs: expanded once, but each candidate is charged its own conversion -------
read -r PAYLOAD RAW_DECL TU_DECL < <(python3 -c "
import json,sys; d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['raw_bytes'], d['tu_count'])" "$CELL/corpus.json")
zstd -d --long=31 -q -c "$CELL/$PAYLOAD" | tar -xf - -C "$RUN_ROOT/input"
awk -F'\t' -v d="$RUN_ROOT/input/" 'NR>1{print d $2}' "$CELL/manifest.tsv" > "$RUN_ROOT/input/manifest.txt"
TUS=$(wc -l < "$RUN_ROOT/input/manifest.txt")
[[ $TUS -eq $TU_DECL ]] || { echo "TU count $TUS != declared $TU_DECL" >&2; exit 5; }
PROBE=$(( TUS < 112 ? TUS : 112 ))
head -n "$PROBE" "$RUN_ROOT/input/manifest.txt" > "$RUN_ROOT/input/manifest.probe.txt"

P29C=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 4 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
      --blob-zstd-workers 2 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
BLIND=(); (( PROBE < TUS )) && BLIND=(--open-final-entropy)
GRZP=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8)
TFMT='%e %U %S %M'   # wall, user, sys, peak RSS KiB

printf 'rep\tmakespan_s\tload_before\tload_after\tp29_e\tp29_user\tp29_sys\tp29_rssKiB\tgrz_e\tgrz_user\tgrz_sys\tgrz_rssKiB\tp29_wire\tgrz_wire\tp29_wire_matches_gate\tgrz_roundtrip\n' \
    > "$RUN_ROOT/reps.tsv"

for rep in $(seq 1 "$REPS"); do
    R=$RUN_ROOT/rep$rep; mkdir -p "$R"/{p29,grz}
    LOAD_BEFORE=$(cut -d' ' -f1 /proc/loadavg)

    # ---- condition 1 + 3: each candidate's whole path, including its own input
    # conversion and (for P29) BOTH passes, is inside its candidate clock.
    cat > "$R/p29.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
taskset -c $P29_CORES $P29 --manifest $RUN_ROOT/input/manifest.probe.txt ${P29C[*]} \\
    --mixed-dump-prefix $R/p29/plan > $R/p29/plan.out 2> $R/p29/plan.err
taskset -c $P29_CORES $P29 --manifest $RUN_ROOT/input/manifest.probe.txt ${P29C[*]} \\
    --literal-group-prefix $R/p29/plan --literal-group-tus 112 --stable-root-tags ${BLIND[*]:-} \\
    --literal-group-workers 8 --literal-group-skip-zstd10 \\
    --literal-group-wire $R/p29/literal.wire > $R/p29/grouped.out 2> $R/p29/grouped.err
SH
    cat > "$R/grz.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
tr '\\n' '\\0' < $RUN_ROOT/input/manifest.probe.txt | xargs -0 cat > $R/grz/probe.ii
taskset -c $GRZ_CORES $GRZ tu $RUN_ROOT/input/manifest.probe.txt $R/grz/probe.tu > /dev/null
taskset -c $GRZ_CORES $GRZ enc $R/grz/probe.ii $R/grz/probe.grz -u $R/grz/probe.tu \\
    ${GRZP[*]} --curve $R/grz/curve.tsv > $R/grz/enc.out 2> $R/grz/enc.err
SH
    chmod +x "$R/p29.sh" "$R/grz.sh"

    T0=$(date +%s.%N)
    /usr/bin/time -f "$TFMT" -o "$R/p29.res" "$R/p29.sh" & PID29=$!
    /usr/bin/time -f "$TFMT" -o "$R/grz.res" "$R/grz.sh" & PIDGZ=$!
    ST29=0; STGZ=0
    wait $PID29 || ST29=$?
    wait $PIDGZ || STGZ=$?
    T1=$(date +%s.%N)
    (( ST29 == 0 && STGZ == 0 )) || { echo "rep$rep candidate failed: p29=$ST29 grz=$STGZ" >&2; exit 6; }
    MAKESPAN=$(echo "$T1-$T0" | bc)
    LOAD_AFTER=$(cut -d' ' -f1 /proc/loadavg)

    read -r P29_E P29_U P29_S P29_M < "$R/p29.res"
    read -r GRZ_E GRZ_U GRZ_S GRZ_M < "$R/grz.res"
    for v in "$P29_E" "$P29_U" "$P29_S" "$P29_M" "$GRZ_E" "$GRZ_U" "$GRZ_S" "$GRZ_M"; do num "$v" resource >/dev/null; done
    P29_WIRE=$(num "$(grep -o 'TOTAL=[0-9]*' "$R/p29/grouped.out" | head -1 | cut -d= -f2)" p29_wire)
    GRZ_WIRE=$(num "$(cut -f2 "$R/grz/enc.out")" grz_wire)

    # ---- condition 4: hard gates on the produced wires
    if cmp -s "$R/p29/literal.wire" "$IDDIR/prefix/literal.wire"; then GATE=MATCH; else GATE=MISMATCH; fi
    taskset -c "$GRZ_CORES" "$GRZ" dec "$R/grz/probe.grz" "$R/grz/replay.ii" -j 1 > /dev/null 2> "$R/grz/dec.err"
    if cmp -s "$R/grz/probe.ii" "$R/grz/replay.ii"; then RT=EXACT; else RT=MISMATCH; fi
    rm -f "$R/grz/replay.ii" "$R/grz/probe.ii" "$R"/p29/plan.*.raw
    [[ $GATE == MATCH && $RT == EXACT ]] || { echo "rep$rep gate failure: wire=$GATE roundtrip=$RT" >&2; exit 7; }

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$rep" "$MAKESPAN" "$LOAD_BEFORE" "$LOAD_AFTER" \
        "$P29_E" "$P29_U" "$P29_S" "$P29_M" "$GRZ_E" "$GRZ_U" "$GRZ_S" "$GRZ_M" \
        "$P29_WIRE" "$GRZ_WIRE" "$GATE" "$RT" >> "$RUN_ROOT/reps.tsv"
done

PROBE_RAW=$(awk -F'\t' -v n="$PROBE" 'NR>1 && NR<=n+1 {s+=$3} END{print s}' "$CELL/manifest.tsv")
{
    printf 'project\t%s\nprofile\t%s\ntotal_tus\t%s\nprobe_tus\t%s\nprobe_raw_bytes\t%s\n' \
        "$P" "$PR" "$TUS" "$PROBE" "$PROBE_RAW"
    printf 'p29_cores\t%s\ngrz_cores\t%s\nreps\t%s\n' "$P29_CORES" "$GRZ_CORES" "$REPS"
    printf 'runner_kind\toffline-census (races both probes; NOT production policy B)\n'
    printf 'p29_passes_charged\tplan+grouped (two-pass; one-pass P29 would replace both)\n'
    printf 'input_charging\tper-candidate (GRZ pays concat+TU map, P29 pays its own open+intern)\n'
    printf 'p29_interner\tresearch (not fast M5; not the product limit)\n'
    printf 'payload\t%s\npayload_sha256\t%s\ncorpus_json_sha256\t%s\nmanifest_tsv_sha256\t%s\n' \
        "$PAYLOAD" "$(sha256sum "$CELL/$PAYLOAD"|cut -d' ' -f1)" \
        "$(sha256sum "$CELL/corpus.json"|cut -d' ' -f1)" "$(sha256sum "$CELL/manifest.tsv"|cut -d' ' -f1)"
    printf 'p29_src_sha256\t%s\np29_bin_sha256\t%s\n' "$(sha256sum "$P29SRC"|cut -d' ' -f1)" "$(sha256sum "$P29"|cut -d' ' -f1)"
    printf 'grz_src_sha256\t%s\ngrz_bin_sha256\t%s\n' "$(sha256sum "$GRZSRC"|cut -d' ' -f1)" "$(sha256sum "$GRZ"|cut -d' ' -f1)"
    printf 'runner_sha256\t%s\n' "$(sha256sum "${BASH_SOURCE[0]}"|cut -d' ' -f1)"
    printf 'zstd\t%s\nhost\t%s\ncompleted_utc\t%s\n' \
        "$(zstd --version 2>&1 | grep -oP 'v[0-9.]+' | head -1)" "$(hostname)" "$(date -u +%FT%TZ)"
} > "$RUN_ROOT/run.meta"

find "$RUN_ROOT/input" -mindepth 1 -delete
echo "RESOURCE_DONE $P/$PR reps=$REPS -> $RUN_ROOT"
