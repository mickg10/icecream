# Real-machine stress drivers

Used for the results in [DEPLOYMENT-MATRIX.md](../../research/reports/DEPLOYMENT-MATRIX.md) section 7. These are
environment-specific harnesses, parameterized via:

- `STRESS_SCHED_IP`   LAN IP of the host running the scheduler(s) + c-role
- `STRESS_FARM_SSH`   `user@host` of the second farm machine (needs
                      passwordless `sudo -n` for the native root iceccd)
- `STRESS_ENV_TAR`    an `icecc-create-env` tarball for `ICECC_VERSION`

Build the farm container image from `Dockerfile` (`docker build -t
icefarm:stress .` with `bin/{o,x}/` populated with iceccd/icecc/
icecc-scheduler of the two versions under test), generate the workload
with `gen-workload.py`, then run `matrix.sh <A|B|C|D|E|F>` or
`gtest.sh <G1|G2|G3>`.  See the file headers for topology details.
