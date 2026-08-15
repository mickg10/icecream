# corpus15 -- simdjson SIMD JSON parser, developer mode.
# 3 of 156 TUs fail to preprocess (developer-mode targets that need generated inputs);
# they are dropped, so 153 is the expected count, not a regression.
PROJECT=simdjson
CHECKOUT=simdjson
GIT_URL=https://github.com/simdjson/simdjson.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=153
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DSIMDJSON_DEVELOPER_MODE=ON)
