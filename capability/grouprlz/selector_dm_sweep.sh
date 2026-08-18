#!/usr/bin/env bash
cd ~/selbind
for p in "$@"; do
  for pr in debian-gcc conan-gcc linuxbrew fedora-clang-libcxx; do
    ./dmcell.sh "$p" "$pr" || echo "CELL_FAIL $p/$pr"
  done
done
echo DMSWEEP_DONE
