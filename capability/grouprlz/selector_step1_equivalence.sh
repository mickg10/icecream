set +e
MX=${MX:-$HOME/ictmp/ii-matrix}
# Build from the branch (selector_build_codec50_sink.sh) rather than a prebuilt binary.
BIN=${BIN:-$(dirname "${BASH_SOURCE[0]:-$0}")/build/codec50-sink}
[ -x "$BIN" ] || { echo "build codec50-sink first: ./selector_build_codec50_sink.sh" >&2; exit 1; }
B="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3 --stable-root-tags"
printf "cell\tn\tplan_cf\tod_cf\tcf_identical\tfc_identical\tplan_wall\tod_wall\n"
for cell in "re2 debian-gcc" "fmt debian-gcc" "cereal debian-gcc" "leveldb debian-gcc" "nlohmann-json debian-gcc" "spdlog debian-gcc" "re2 fedora-clang-libcxx" "fmt linuxbrew" "cereal conan-gcc" "leveldb fedora-clang-libcxx"; do
  set -- $cell; P=$1; PR=$2; T=/tmp/od.$P.$PR; rm -rf $T; mkdir -p $T/ii
  J=$MX/$P/$PR/corpus.json
  read -r REL SHA N < <(python3 -c "
import json;d=json.load(open(\"$J\"))
print(d[\"payload\"][\"path\"], d[\"payload\"][\"sha256\"], d[\"tu_count\"])")
  [ "$(sha256sum $MX/$P/$PR/$REL | cut -d\  -f1)" = "$SHA" ] || { echo "$P.$PR SHA MISMATCH"; continue; }
  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C $T/ii
  find $T/ii -name "*.ii" | sort > $T/man; : > $T/man4; for i in 1 2 3 4; do cat $T/man >> $T/man4; done
  S1=$(date +%s.%N)
  nice -n 8 $BIN --manifest $T/man4 $B --mixed-dump-prefix $T/pl > /dev/null 2>&1
  nice -n 8 $BIN --manifest $T/man4 $B --literal-group-prefix $T/pl --literal-group-tus 1 --literal-group-workers 8 --literal-group-skip-zstd10 --cf-sink $T/plan.cf --fc-sink $T/plan.fc --sink-build-tus $N > $T/plan.out 2>&1
  S2=$(date +%s.%N)
  nice -n 8 $BIN --manifest $T/man4 $B --literal-ondemand --literal-group-skip-zstd10 --cf-sink $T/od.cf --fc-sink $T/od.fc --sink-build-tus $N > $T/od.out 2>&1
  S3=$(date +%s.%N)
  ok1=NO; ok2=NO
  grep -q byte-exact=OK $T/plan.out && grep -q byte-exact=OK $T/od.out || { echo "$P.$PR NOT byte-exact"; rm -rf $T; continue; }
  cmp -s $T/plan.cf $T/od.cf && ok1=YES; cmp -s $T/plan.fc $T/od.fc && ok2=YES
  printf "%s.%s\t%s\t%s\t%s\t%s\t%s\t%.2f\t%.2f\n" "$P" "$PR" "$N" "$(stat -c %s $T/plan.cf)" "$(stat -c %s $T/od.cf)" "$ok1" "$ok2" "$(echo "$S2-$S1"|bc)" "$(echo "$S3-$S2"|bc)"
  rm -rf $T
done
