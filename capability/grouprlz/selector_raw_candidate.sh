set +e
MX=$HOME/ictmp/ii-matrix; BIN=/tmp/tagreg/build/codec50-sink
B="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3 --stable-root-tags"
printf "cell\tn\tGLOBAL_S1_cf\tRAW_cf\tRAW_vs_GLOBAL\n"
for cell in "re2 debian-gcc" "fmt debian-gcc" "cereal debian-gcc" "leveldb debian-gcc" "nlohmann-json debian-gcc" "spdlog debian-gcc"; do
  set -- $cell; P=$1; PR=$2; T=/tmp/raw.$P; rm -rf $T; mkdir -p $T/ii
  J=$MX/$P/$PR/corpus.json
  read -r REL SHA N < <(python3 -c "
import json;d=json.load(open(\"$J\"))
print(d[\"payload\"][\"path\"], d[\"payload\"][\"sha256\"], d[\"tu_count\"])")
  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C $T/ii
  find $T/ii -name "*.ii"|sort>$T/man; :>$T/man4; for i in 1 2 3 4; do cat $T/man>>$T/man4; done
  $BIN --manifest $T/man4 $B --literal-ondemand --literal-group-skip-zstd10 --cf-sink $T/g.cf --fc-sink $T/g.fc >$T/g.out 2>&1
  $BIN --manifest $T/man4 $B --v1 --literal-ondemand --literal-group-skip-zstd10 --cf-sink $T/r.cf --fc-sink $T/r.fc >$T/r.out 2>&1
  grep -q byte-exact=OK $T/g.out && grep -q byte-exact=OK $T/r.out || { echo "$P NOT byte-exact"; rm -rf $T; continue; }
  G=$(stat -c %s $T/g.cf); R=$(stat -c %s $T/r.cf)
  printf "%s\t%s\t%s\t%s\t%+.1f%%\n" "$P" "$N" "$G" "$R" "$(echo "scale=4;($R-$G)*100/$G"|bc)"
  rm -rf $T
done
