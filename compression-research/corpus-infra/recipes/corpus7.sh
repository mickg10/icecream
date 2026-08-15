# corpus7 -- fmt (fmtlib), string formatting library.  Tests enabled so the corpus
# gets both the library TUs and the (header-heavy) test TUs.
PROJECT=fmt
CHECKOUT=fmt
GIT_URL=https://github.com/fmtlib/fmt.git
GIT_REF=              # empty = shallow clone of default-branch tip
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=50
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DFMT_TEST=ON -DFMT_DOC=OFF -DFMT_INSTALL=OFF -DFMT_FUZZ=OFF -DFMT_CUDA_TEST=OFF)
