#!/usr/bin/env bash
cd ~/selbind
for p in re2 fmt leveldb cereal nlohmann-json spdlog range-v3 catch2 rocksdb opencv eigen; do
  for pr in debian-gcc conan-gcc linuxbrew fedora-clang-libcxx; do
    printf "[%s] load=%s " "$(date -u +%H:%M:%S)" "$(cut -d" " -f1 /proc/loadavg)"
    ./pbcell.sh "$p" "$pr" || echo "PBCELL_FAIL $p/$pr"
  done
done
echo PBSWEEP_DONE
