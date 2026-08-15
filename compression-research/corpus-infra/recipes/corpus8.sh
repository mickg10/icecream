# corpus8 -- spdlog, fast header-only/compiled C++ logging library.
PROJECT=spdlog
CHECKOUT=spdlog
GIT_URL=https://github.com/gabime/spdlog.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=34
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DSPDLOG_BUILD_TESTS=ON -DSPDLOG_BUILD_EXAMPLE=ON -DSPDLOG_BUILD_BENCH=OFF)
