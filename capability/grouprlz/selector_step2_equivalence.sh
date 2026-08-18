set +e
MX=$HOME/ictmp/ii-matrix; S1=$HOME/selbind/p29build/codec50-sink; S2=$HOME/selbind/p29build/codec50-sink-s2
BASE="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
printf "cell\tn\tstable_cf\tstable_ident\tlegacy_total\tlegacy_ident\n"
for cell in "re2 debian-gcc" "fmt debian-gcc" "cereal debian-gcc" "leveldb debian-gcc" "nlohmann-json debian-gcc" "spdlog debian-gcc" "re2 fedora-clang-libcxx" "fmt linuxbrew" "cereal conan-gcc" "leveldb fedora-clang-libcxx"; do
  set -- $cell; P=$1; PR=$2; T=/tmp/s2.$P.$PR; rm -rf $T; mkdir -p $T/ii
  J=$MX/$P/$PR/corpus.json
  read -r REL SHA N < <(python3 -c "
import json;d=json.load(open(\"$J\"))
print(d[\"payload\"][\"path\"], d[\"payload\"][\"sha256\"], d[\"tu_count\"])")
  [ "$(sha256sum $MX/$P/$PR/$REL | cut -d\  -f1)" = "$SHA" ] || { echo "$P.$PR SHA MISMATCH"; continue; }
  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C $T/ii
  find $T/ii -name "*.ii" | sort > $T/man; : > $T/man4; for i in 1 2 3 4; do cat $T/man >> $T/man4; done
  # A) product path: stable tags + on-demand literals, compare the physical streams
  for V in a b; do B=$([ $V = a ] && echo $S1 || echo $S2)
    nice -n 8 $B --manifest $T/man4 $BASE --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --cf-sink $T/$V.cf --fc-sink $T/$V.fc --sink-build-tus $N > $T/$V.out 2>&1
    grep -q byte-exact=OK $T/$V.out || echo "$P.$PR variant $V NOT byte-exact"; done
  SI=NO; cmp -s $T/a.cf $T/b.cf && cmp -s $T/a.fc $T/b.fc && SI=YES
  # B) legacy flat Root namespace (no --stable-root-tags): accounting TOTAL must be unchanged
  for V in c d; do B=$([ $V = c ] && echo $S1 || echo $S2)
    nice -n 8 $B --manifest $T/man4 $BASE --literal-ondemand --literal-group-skip-zstd10 > $T/$V.out 2>&1
    grep -q byte-exact=OK $T/$V.out || echo "$P.$PR legacy $V NOT byte-exact"; done
  LA=$(grep -o "TOTAL=[0-9]*" $T/c.out | head -1); LB=$(grep -o "TOTAL=[0-9]*" $T/d.out | head -1)
  LI=NO; [ "$LA" = "$LB" ] && [ -n "$LA" ] && LI=YES
  printf "%s.%s\t%s\t%s\t%s\t%s\t%s\n" "$P" "$PR" "$N" "$(stat -c %s $T/b.cf)" "$SI" "${LB#TOTAL=}" "$LI"
  rm -rf $T
done
