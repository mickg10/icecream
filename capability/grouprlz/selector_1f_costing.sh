set +e
MX=$HOME/ictmp/ii-matrix; BIN=/tmp/tagreg/build/codec50-sink
B='--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3 --stable-root-tags'
printf 'cell\tTUs\traw_cum\tglobal_cum\tRAW_wins\tGLOBAL_wins\tRAW_win_pct\n'
for cell in 're2 debian-gcc' 'fmt debian-gcc' 'cereal debian-gcc' 'leveldb debian-gcc' 'spdlog debian-gcc'; do
  set -- $cell; P=$1; PR=$2; T=/tmp/sc.$P; rm -rf $T; mkdir -p $T/ii
  J=$MX/$P/$PR/corpus.json
  read -r REL SHA N < <(python3 -c "
import json;d=json.load(open('$J'))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])")
  [ "$(sha256sum $MX/$P/$PR/$REL | cut -d' ' -f1)" = "$SHA" ] || { echo "$P SHA MISMATCH"; continue; }
  zstd -d --long=31 -c $MX/$P/$PR/$REL 2>/dev/null | tar -xf - -C $T/ii
  find $T/ii -name '*.ii'|sort>$T/man; M=$(wc -l < $T/man)
  [ "$M" = "$N" ] || { echo "$P manifest $M != $N"; rm -rf $T; continue; }
  :>$T/man4; for i in 1 2 3 4; do cat $T/man>>$T/man4; done
  $BIN --manifest $T/man4 $B --literal-ondemand --literal-group-skip-zstd10 --route-s1 1 --selector-tsv $T/sel.tsv --cf-sink $T/s.cf --fc-sink $T/s.fc >$T/o 2>&1
  grep -q byte-exact=OK $T/o || { echo "$P NOT byte-exact"; rm -rf $T; continue; }
  L=$(grep -o 'SELECTOR per-TU costing:.*' $T/o)
  R=$(echo "$L"|sed 's/.*raw_cum=\([0-9]*\).*/\1/'); G=$(echo "$L"|sed 's/.*global_cum=\([0-9]*\).*/\1/')
  RW=$(echo "$L"|sed 's/.*RAW=\([0-9]*\).*/\1/'); GW=$(echo "$L"|sed 's/.*GLOBAL_S1=\([0-9]*\).*/\1/')
  TU=$((N*4))
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%.1f%%\n' "$P" "$TU" "$R" "$G" "$RW" "$GW" "$(echo "scale=4;$RW*100/$TU"|bc)"
  cp $T/sel.tsv /tmp/tagreg/sel.$P.tsv; rm -rf $T
done
