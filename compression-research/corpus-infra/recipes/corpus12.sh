# corpus12 -- Eigen header-only linear algebra, test suite.
# GOTCHA: Eigen TUs are enormous after preprocessing (~5.4 MB of .ii each, the
# largest per-TU expansion in the whole set).  The full 1542-TU test suite is ~8 GB
# of .ii, so the corpus is capped with --limit 650 (~3.5 GB) to stay comparable in
# size with the other corpora.  Raise PP_LIMIT if you want the whole suite.
PROJECT=Eigen
CHECKOUT=eigen
GIT_URL=https://gitlab.com/libeigen/eigen.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=650
EXPECTED_TU=650
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DEIGEN_BUILD_TESTING=ON -DBUILD_TESTING=ON -DEIGEN_BUILD_DOC=OFF -DEIGEN_BUILD_PKGCONFIG=OFF)
