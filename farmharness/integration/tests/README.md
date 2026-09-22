# Local integration harness tests

These tests exercise the Python integration harness locally. They do not
connect to a farm, run Docker, or execute an integration suite against remote
workers. The test suite uses Python 3, pytest, and otherwise Python's standard
library. Install pytest with `python3 -m pip install pytest`. The suite also
uses GNU Make, Git, Bash, and a C++ compiler; Linux namespace tests skip when
the host does not permit user/PID namespaces. Run from the source tree:

```sh
ICEFARM_TMPDIR=/scratch/icefarm make test-harness-fast
ICEFARM_TMPDIR=/scratch/icefarm make test-harness-thorough
```

`ICEFARM_TMPDIR` is required for farm integration, harness, and formal Make
targets. Provision an existing writable absolute directory before invoking
them; the Make guard checks it without creating it. The setting is exported as
`TMPDIR`, `TMP`, `TEMP`, and `TEMPDIR` for child processes, and disables Python
bytecode writes. Provision it first, for example: `mkdir -p /scratch/icefarm`.

This routes Python and harness temporary files only. It does not relocate the
Docker daemon's `DockerRootDir` or native compilation object files. For large
native builds, use an out-of-tree build directory on the desired storage.
The fast target skips tests marked `thorough`. The thorough target runs the
complete test directory, including the slower scale and namespace revalidation
checks. A previous complete run took 540.19 seconds, of which the retained
namespace revalidation test took 450.915 seconds; time varies with the machine.
Some retained-corpus probes depend on local external data and skip when those
files are unavailable; they do not make the rest of the test suite dependent
on a farm or Docker.

Bare pytest behavior is unchanged: `python3 -m pytest
farmharness/integration/tests` still runs the entire directory. Both make
targets also avoid pytest's cache and Python bytecode writes.
