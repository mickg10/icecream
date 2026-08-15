# corpus16 -- cereal header-only C++11 serialization library, tests + sandbox.
PROJECT=cereal
CHECKOUT=cereal
GIT_URL=https://github.com/USCiLab/cereal.git
GIT_REF=
CLONE_ARGS=--recursive
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=84
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DBUILD_TESTS=ON -DBUILD_SANDBOX=ON -DSKIP_PERFORMANCE_COMPARISON=ON -DCEREAL_INSTALL=OFF)
