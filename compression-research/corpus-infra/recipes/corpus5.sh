# corpus5 -- OpenCV, default module set, tests + perf tests.
# GOTCHA: OpenCV emits generated sources (modules/*/opencl_kernels_*.cpp) and generated
# headers that the .ii need.  cmake alone does not create them, so a partial ninja run
# over just the generated-file targets has to happen between configure and -E replay.
PROJECT=OpenCV
CHECKOUT=opencv
GIT_URL=https://github.com/opencv/opencv.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=1506
CMAKE_ARGS=(
  -DBUILD_TESTS=ON -DBUILD_PERF_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF
  -DBUILD_JAVA=OFF -DBUILD_SHARED_LIBS=ON
)
# Materialize generated sources/headers before the -E replay.
postconfigure () {
  local b="$1" tgts
  tgts=$( (cd "$b" && ninja -t targets all 2>/dev/null) \
          | awk -F': ' '{print $1}' \
          | grep -E 'opencl_kernels_[a-z0-9_]*\.cpp$|version_string\.inc$|/opencv2/.*\.hpp$' \
          | sort -u )
  [ -n "$tgts" ] || { echo "  [opencv] no generated targets found -- skipping codegen"; return 0; }
  # shellcheck disable=SC2086
  nice -19 ninja -C "$b" -j12 -k 0 $tgts
}
