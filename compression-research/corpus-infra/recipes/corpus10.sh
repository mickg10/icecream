# corpus10 -- nlohmann/json single-header JSON library, test suite.
PROJECT=nlohmann-json
CHECKOUT=json
GIT_URL=https://github.com/nlohmann/json.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=99
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DJSON_BuildTests=ON -DJSON_Install=OFF)
