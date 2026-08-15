# corpus9 -- Catch2 unit-test framework.  Development build + examples + extra tests
# gives ~857 small TUs that all include the same large Catch2 headers: the densest
# cross-TU redundancy in the set.
PROJECT=Catch2
CHECKOUT=catch2
GIT_URL=https://github.com/catchorg/Catch2.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=857
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DCATCH_DEVELOPMENT_BUILD=ON -DBUILD_TESTING=ON -DCATCH_BUILD_EXAMPLES=ON -DCATCH_BUILD_EXTRA_TESTS=ON)
